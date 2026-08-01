#pragma once

#include <Poco/Net/HTTPRequestHandler.h>
#include <Poco/Net/HTTPServerRequest.h>
#include <Poco/Net/HTTPServerResponse.h>
#include <Poco/URI.h>
#include <Poco/Logger.h>
#include <Poco/Timestamp.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Stringifier.h>
#include <Poco/NumberParser.h>

#include "request_counter.h"
#include "../openai/async_openai_manager.h"

#include <sstream>
#include <string>

namespace handlers {

/**
 * GET /api/v1/async_ai_status?request_id={id}
 * Возвращает статус асинхронной AI-задачи: request_id, status, start_time_ms,
 * retries, bytes_sent. 404, если id неизвестен.
 */
class AsyncAIStatusHandler : public Poco::Net::HTTPRequestHandler {
public:
    void handleRequest(Poco::Net::HTTPServerRequest& request,
                       Poco::Net::HTTPServerResponse& response) override {
        Poco::Timestamp start;
        if (g_httpRequests) g_httpRequests->inc();

        response.setContentType("application/json");
        auto& logger = Poco::Logger::get("Server");

        if (request.getMethod() != "GET") {
            response.setStatus(Poco::Net::HTTPResponse::HTTP_METHOD_NOT_ALLOWED);
            std::ostream& ostr = response.send();
            ostr << R"({"error":"Method not allowed"})";
            if (g_httpErrors) g_httpErrors->inc();
            logDuration(start, request, 405, logger);
            return;
        }

        try {
            Poco::URI uri("http://localhost" + request.getURI());
            std::string ridStr = getQueryParam(uri, "request_id");

            if (ridStr.empty()) {
                response.setStatus(Poco::Net::HTTPResponse::HTTP_BAD_REQUEST);
                std::ostream& ostr = response.send();
                ostr << R"({"error":"request_id query parameter is required"})";
                if (g_httpErrors) g_httpErrors->inc();
                logDuration(start, request, 400, logger);
                return;
            }

            int64_t id;
            try {
                id = Poco::NumberParser::parse64(ridStr);
            } catch (...) {
                response.setStatus(Poco::Net::HTTPResponse::HTTP_BAD_REQUEST);
                std::ostream& ostr = response.send();
                ostr << R"({"error":"request_id must be an integer"})";
                if (g_httpErrors) g_httpErrors->inc();
                logDuration(start, request, 400, logger);
                return;
            }

            openai::AsyncJob job;
            if (!openai::AsyncOpenAIManager::instance().getJob(id, job)) {
                response.setStatus(Poco::Net::HTTPResponse::HTTP_NOT_FOUND);
                std::ostream& ostr = response.send();
                ostr << R"({"error":"request_id not found"})";
                if (g_httpErrors) g_httpErrors->inc();
                logDuration(start, request, 404, logger);
                return;
            }

            Poco::JSON::Object jsonResponse;
            jsonResponse.set("request_id", job.id);
            jsonResponse.set("status", statusToString(job.status));
            jsonResponse.set("start_time_ms", static_cast<Poco::Int64>(job.startTime.epochMicroseconds() / 1000));
            jsonResponse.set("retries", job.retries);
            jsonResponse.set("bytes_sent", static_cast<Poco::UInt64>(job.bytesSent));

            response.setStatus(Poco::Net::HTTPResponse::HTTP_OK);
            std::ostream& ostr = response.send();
            Poco::JSON::Stringifier::stringify(jsonResponse, ostr);

            logDuration(start, request, 200, logger);

        } catch (const std::exception& e) {
            response.setStatus(Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR);
            std::ostream& ostr = response.send();
            ostr << R"({"error":"Internal error"})";
            if (g_httpErrors) g_httpErrors->inc();
            logger.error("async_ai_status: %s", e.what());
            logDuration(start, request, 500, logger);
        }
    }

private:
    static std::string statusToString(openai::AsyncStatus s) {
        switch (s) {
            case openai::AsyncStatus::Running:   return "running";
            case openai::AsyncStatus::Completed: return "completed";
            case openai::AsyncStatus::Failed:    return "failed";
        }
        return "running";
    }

    static std::string getQueryParam(const Poco::URI& uri, const std::string& name) {
        for (const auto& p : uri.getQueryParameters()) {
            if (p.first == name) return p.second;
        }
        return "";
    }

    static void logDuration(Poco::Timestamp start, Poco::Net::HTTPServerRequest& request,
                           int status, Poco::Logger& logger) {
        Poco::Timespan duration = Poco::Timestamp() - start;
        double seconds = static_cast<double>(duration.totalMicroseconds()) / 1000000.0;
        if (g_httpDuration) g_httpDuration->observe(seconds);
        logger.information("%d GET /api/v1/async_ai_status from %s, %.2f ms",
                           status, request.clientAddress().toString(), seconds * 1000.0);
    }
};

} // namespace handlers
