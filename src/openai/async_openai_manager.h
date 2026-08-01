#pragma once

#include <Poco/ThreadPool.h>
#include <Poco/Runnable.h>
#include <Poco/Timestamp.h>
#include <Poco/Thread.h>
#include <Poco/Environment.h>
#include <Poco/NumberParser.h>
#include <Poco/Logger.h>

#include "openai_client.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace openai {

class AsyncOpenAIManager;

/**
 * Runnable, который выполняет асинхронную AI-задачу в потоке Poco::ThreadPool.
 * Живёт, пока его держит shared_ptr в AsyncJob (см. AsyncJob::runnable), поэтому
 * delete себя не выполняет.
 */
class AsyncJobRunnable : public Poco::Runnable {
public:
    explicit AsyncJobRunnable(int64_t id) : id_(id) {}
    void run() override;

private:
    int64_t id_;
};

enum class AsyncStatus {
    Running,    // задача выполняется
    Completed,  // завершена успешно, результат в result
    Failed      // завершена с ошибкой, описание в error
};

/** Одна асинхронная задача: метаданные + результат. */
struct AsyncJob {
    int64_t id = 0;
    Poco::Timestamp startTime;              // время старта процесса
    int retries = 0;                        // количество перепосылов запросов к AI
    size_t bytesSent = 0;                   // суммарно отправлено байт в ИИ (за все попытки)
    AsyncStatus status = AsyncStatus::Running;
    std::string result;                     // ответ LLM при успешном завершении
    std::string error;                      // описание ошибки при завершении с ошибкой
    std::string prompt;                     // текст запроса (для повторных перепосылов)
    std::shared_ptr<AsyncJobRunnable> runnable;  // удерживает Runnable живым во время run()
};

/**
 * Менеджер асинхронных AI-запросов.
 * Хранит задачи в памяти (std::unordered_map<int64_t, AsyncJob>), выполняет их
 * в Poco::ThreadPool. Паттерн: submit() -> id, опрос через getJob().
 *
 * Конфигурация (переменные окружения):
 *   OPENAI_ASYNC_MAX_THREADS - размер пула потоков (по умолчанию 4)
 *   OPENAI_MAX_RETRIES       - число повторных перепосылов при ошибке AI (по умолчанию 3)
 *   OPENAI_ASYNC_MAX_JOBS    - максимум хранимых задач (по умолчанию 10000), старые завершённые удаляются
 */
class AsyncOpenAIManager {
public:
    static AsyncOpenAIManager& instance() {
        static AsyncOpenAIManager mgr;
        return mgr;
    }

    /**
     * Создать задачу, сгенерировать id и поставить её в очередь ThreadPool.
     * @param prompt текст запроса к AI
     * @return сгенерированный id (int64)
     */
    int64_t submit(const std::string& prompt) {
        int64_t id = nextId_.fetch_add(1);
        auto runnable = std::make_shared<AsyncJobRunnable>(id);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            AsyncJob job;
            job.id = id;
            job.startTime = Poco::Timestamp();
            job.status = AsyncStatus::Running;
            job.prompt = prompt;
            job.runnable = runnable;
            jobs_.emplace(id, std::move(job));
            pruneIfNeeded();
        }
        try {
            pool_.start(*runnable);
        } catch (const Poco::Exception& e) {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = jobs_.find(id);
            if (it != jobs_.end()) {
                it->second.status = AsyncStatus::Failed;
                it->second.error = "No thread available in pool: " + e.displayText();
            }
        }
        return id;
    }

    /**
     * Получить снимок задачи под блокировкой.
     * @return false, если id неизвестен
     */
    bool getJob(int64_t id, AsyncJob& out) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = jobs_.find(id);
        if (it == jobs_.end()) return false;
        out = it->second;
        return true;
    }

    int maxRetries() const { return maxRetries_; }

    /** Тело воркера: вызывает AI с повторами и обновляет задачу. */
    void execute(int64_t id) {
        auto& logger = Poco::Logger::get("AsyncOpenAIManager");

        std::string prompt;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = jobs_.find(id);
            if (it == jobs_.end()) return;
            prompt = it->second.prompt;
        }

        std::string result, error;
        bool ok = false;
        int attempts = 0;
        for (;;) {
            ++attempts;
            try {
                OpenAIClient client;
                result = client.chatCompletion(prompt);
                ok = true;
                break;
            } catch (const OpenAIAPIException& e) {
                error = e.what();
                // Перепосылаем только транзиентные ошибки: 429 и 5xx.
                if (!isRetryableHttpStatus(e.httpStatus()) || attempts > maxRetries_) {
                    break;
                }
                logger.warning("async AI attempt %d/%d failed for job %lld with HTTP %d: %s",
                               attempts, maxRetries_ + 1,
                               static_cast<long long>(id), e.httpStatus(), error);
                Poco::Thread::sleep(400 * attempts);
            } catch (const std::exception& e) {
                // Не 5xx/429 (ошибка конфигурации, разбора ответа, сети) — не перепосылаем.
                error = e.what();
                break;
            }
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = jobs_.find(id);
            if (it == jobs_.end()) return;
            it->second.bytesSent += prompt.size() * attempts;
            it->second.retries = attempts - 1;
            if (ok) {
                it->second.status = AsyncStatus::Completed;
                it->second.result = result;
                it->second.error.clear();
                logger.information("async AI job %lld completed (attempts=%d)",
                                   static_cast<long long>(id), attempts);
            } else {
                it->second.status = AsyncStatus::Failed;
                it->second.error = error;
                logger.error("async AI job %lld failed after %d attempts: %s",
                             static_cast<long long>(id), attempts, error);
            }
        }
    }

private:
    AsyncOpenAIManager()
        : nextId_(static_cast<int64_t>(Poco::Timestamp().epochMicroseconds() & 0x3FFFFFFFFFFFFFFFLL)),
          pool_(1, asyncPoolSize()),
          maxRetries_(asyncMaxRetries()),
          maxJobs_(asyncMaxJobs()) {
    }

    ~AsyncOpenAIManager() {
        pool_.stopAll();
    }

    AsyncOpenAIManager(const AsyncOpenAIManager&) = delete;
    AsyncOpenAIManager& operator=(const AsyncOpenAIManager&) = delete;

    static int asyncPoolSize() {
        try {
            int v = Poco::NumberParser::parse(Poco::Environment::get("OPENAI_ASYNC_MAX_THREADS", "4"));
            return v >= 1 ? v : 4;
        } catch (...) {
            return 4;
        }
    }

    static bool isRetryableHttpStatus(int status) {
        return status == 429 || (status >= 500 && status < 600);
    }

    static int asyncMaxRetries() {
        try {
            int v = Poco::NumberParser::parse(Poco::Environment::get("OPENAI_MAX_RETRIES", "3"));
            return v >= 0 ? v : 3;
        } catch (...) {
            return 3;
        }
    }

    static size_t asyncMaxJobs() {
        try {
            int v = Poco::NumberParser::parse(Poco::Environment::get("OPENAI_ASYNC_MAX_JOBS", "10000"));
            return v > 0 ? static_cast<size_t>(v) : 10000u;
        } catch (...) {
            return 10000u;
        }
    }

    void pruneIfNeeded() {
        if (jobs_.size() <= maxJobs_) return;
        // Удаляем самые старые завершённые задачи, пока не вернёмся под лимит.
        std::vector<int64_t> finished;
        for (const auto& kv : jobs_) {
            if (kv.second.status != AsyncStatus::Running) finished.push_back(kv.first);
        }
        std::sort(finished.begin(), finished.end(), [&](int64_t a, int64_t b) {
            return jobs_.at(a).startTime < jobs_.at(b).startTime;
        });
        size_t need = jobs_.size() - maxJobs_;
        for (size_t i = 0; i < finished.size() && need > 0; ++i, --need) {
            jobs_.erase(finished[i]);
        }
    }

    mutable std::mutex mutex_;
    std::unordered_map<int64_t, AsyncJob> jobs_;
    std::atomic<int64_t> nextId_;
    Poco::ThreadPool pool_;
    int maxRetries_;
    size_t maxJobs_;
};

inline void AsyncJobRunnable::run() {
    AsyncOpenAIManager::instance().execute(id_);
}

} // namespace openai
