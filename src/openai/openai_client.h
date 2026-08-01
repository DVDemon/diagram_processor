#pragma once

#include <Poco/Net/HTTPClientSession.h>
#include <Poco/Net/HTTPSClientSession.h>
#include <Poco/Net/HTTPRequest.h>
#include <Poco/Net/HTTPResponse.h>
#include <Poco/Net/Context.h>
#include <Poco/URI.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>
#include <Poco/JSON/Array.h>
#include <Poco/StreamCopier.h>
#include <Poco/Environment.h>
#include <Poco/NumberParser.h>
#include <Poco/Logger.h>

#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>

namespace openai {

/**
 * Ошибка OpenAI-совместимого API с HTTP-статусом ответа.
 * Позволяет отличать транзиентные ошибки (429, 5xx — допустимо перепосылать)
 * от постоянных (4xx — перепосылать бессмысленно).
 */
class OpenAIAPIException : public std::runtime_error {
public:
    OpenAIAPIException(int httpStatus, const std::string& message)
        : std::runtime_error(message), httpStatus_(httpStatus) {
    }
    int httpStatus() const noexcept { return httpStatus_; }

private:
    int httpStatus_;
};

/**
 * Client for OpenAI-compatible API (OpenAI, DeepSeek, etc.).
 * Configuration via environment variables:
 *   OPENAI_API_KEY     - API token (required)
 *   OPENAI_API_URL     - Base URL (default: https://api.deepseek.com)
 *   OPENAI_MODEL       - Model name (default: deepseek-chat)
 *   OPENAI_SYSTEM_PROMPT - System prompt (optional)
 *   OPENAI_TIMEOUT     - Timeout in seconds (default: 60)
 *   OPENAI_SSL_VERIFY  - "0" or "false" to disable cert verification (default: disabled for compatibility)
 *
 * Транспорт выбирается автоматически по схеме в OPENAI_API_URL:
 *   https:// -> HTTPSClientSession (TLS, с проверкой сертификатов по OPENAI_SSL_VERIFY),
 *   http://  -> HTTPClientSession  (plain HTTP, подходит для локальных серверов без TLS).
 *
 * HTTP-ошибки API (любой статус ≠ 200) выбрасываются как OpenAIAPIException,
 * в котором есть httpStatus() — по нему вызывающий может решить, перепосылать ли запрос.
 */
class OpenAIClient {
public:
    OpenAIClient()
        : apiKey_(Poco::Environment::get("OPENAI_API_KEY", "")),
          baseUrl_(Poco::Environment::get("OPENAI_API_URL", "https://api.deepseek.com")),
          model_(Poco::Environment::get("OPENAI_MODEL", "deepseek-chat")),
          systemPrompt_(Poco::Environment::get("OPENAI_SYSTEM_PROMPT", "")),
          verifySsl_(parseBool(Poco::Environment::get("OPENAI_SSL_VERIFY", "false"))),
          timeoutSec_(parseTimeout(Poco::Environment::get("OPENAI_TIMEOUT", "60"))) {
    }

    /**
     * Send chat completion request.
     * @param userMessage User prompt text
     * @param systemPromptOverride Optional override for system prompt (uses OPENAI_SYSTEM_PROMPT if empty)
     * @return Response text from the model
     */
    std::string chatCompletion(const std::string& userMessage,
                              const std::string& systemPromptOverride = "") const {
        if (apiKey_.empty()) {
            throw std::runtime_error("OPENAI_API_KEY is not set");
        }

        Poco::URI uri(baseUrl_);
        std::string path = uri.getPath();
        if (path.empty() || path == "/") {
            path = "/v1/chat/completions";
        } else {
            path += (path.back() == '/') ? "chat/completions" : "/chat/completions";
        }

        std::string systemPrompt = systemPromptOverride.empty() ? systemPrompt_ : systemPromptOverride;

        Poco::JSON::Array messages;
        if (!systemPrompt.empty()) {
            Poco::JSON::Object sysMsg;
            sysMsg.set("role", "system");
            sysMsg.set("content", systemPrompt);
            messages.add(sysMsg);
        }
        Poco::JSON::Object userMsg;
        userMsg.set("role", "user");
        userMsg.set("content", userMessage);
        messages.add(userMsg);

        Poco::JSON::Object body;
        body.set("model", model_);
        body.set("messages", messages);

        std::stringstream bodyStream;
        body.stringify(bodyStream);
        std::string bodyStr = bodyStream.str();

        // Автовыбор транспорта по схеме URL: http:// -> plain HTTP, https:// -> TLS.
        std::unique_ptr<Poco::Net::HTTPClientSession> session;
        if (uri.getScheme() == "http") {
            session = std::make_unique<Poco::Net::HTTPClientSession>(uri.getHost(), uri.getPort());
        } else {
            Poco::Net::Context::Ptr sslContext;
            if (verifySsl_) {
                sslContext = new Poco::Net::Context(Poco::Net::Context::CLIENT_USE, "", "", "",
                                                    Poco::Net::Context::VERIFY_STRICT);
            } else {
                sslContext = new Poco::Net::Context(Poco::Net::Context::CLIENT_USE, "", "", "",
                                                    Poco::Net::Context::VERIFY_NONE);
            }
            session = std::make_unique<Poco::Net::HTTPSClientSession>(uri.getHost(), uri.getPort(), sslContext);
        }
        session->setTimeout(Poco::Timespan(timeoutSec_, 0));

        Poco::Net::HTTPRequest req(Poco::Net::HTTPRequest::HTTP_POST, path,
                                   Poco::Net::HTTPMessage::HTTP_1_1);
        req.setContentType("application/json");
        req.setContentLength(static_cast<std::streamsize>(bodyStr.size()));
        req.set("Authorization", "Bearer " + apiKey_);

        session->sendRequest(req) << bodyStr;

        Poco::Net::HTTPResponse res;
        std::istream& rs = session->receiveResponse(res);

        std::string responseBody;
        Poco::StreamCopier::copyToString(rs, responseBody);

        if (res.getStatus() != Poco::Net::HTTPResponse::HTTP_OK) {
            auto& logger = Poco::Logger::get("OpenAIClient");
            logger.error("OpenAI API error: %d - %s", static_cast<int>(res.getStatus()), responseBody);
            throw OpenAIAPIException(static_cast<int>(res.getStatus()),
                                     "OpenAI API error: " + std::to_string(res.getStatus()) + " - " + responseBody);
        }

        return parseChatResponse(responseBody);
    }

    const std::string& getSystemPrompt() const { return systemPrompt_; }
    const std::string& getModel() const { return model_; }
    int getTimeoutSec() const { return timeoutSec_; }

private:
    static bool parseBool(const std::string& s) {
        std::string lower;
        for (char c : s) {
            lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return lower == "1" || lower == "true" || lower == "yes";
    }

    static int parseTimeout(const std::string& s) {
        try {
            int val = Poco::NumberParser::parse(s);
            return val > 0 ? val : 60;
        } catch (...) {
            return 60;
        }
    }

    static std::string parseChatResponse(const std::string& json) {
        try {
            Poco::JSON::Parser parser;
            auto result = parser.parse(json);
            auto obj = result.extract<Poco::JSON::Object::Ptr>();
            auto choices = obj->getArray("choices");
            if (!choices || choices->size() == 0) {
                throw std::runtime_error("No choices in OpenAI response");
            }
            auto firstChoice = choices->getObject(0);
            auto message = firstChoice->getObject("message");
            if (!message) {
                throw std::runtime_error("No message in OpenAI response");
            }
            return message->getValue<std::string>("content");
        } catch (const Poco::Exception& e) {
            throw std::runtime_error("Failed to parse OpenAI response: " + e.displayText());
        }
    }

    std::string apiKey_;
    std::string baseUrl_;
    std::string model_;
    std::string systemPrompt_;
    bool verifySsl_;
    int timeoutSec_;
};

} // namespace openai
