#pragma once

#include <Poco/Net/HTTPRequestHandler.h>
#include <Poco/Net/HTTPServerRequest.h>
#include <Poco/Net/HTTPServerResponse.h>
#include <Poco/Logger.h>
#include <Poco/Timestamp.h>
#include <Poco/JSON/Parser.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Stringifier.h>
#include <Poco/StreamCopier.h>
#include <Poco/Environment.h>

#include "request_counter.h"
#include "../openai/async_openai_manager.h"

#include <sstream>
#include <string>

namespace handlers {

/**
 * POST /api/v1/process_with_ai_async
 * Запускает асинхронную AI-задачу и сразу возвращает request_id (HTTP 202).
 * Статус и результат опрашиваются через /api/v1/async_ai_status и /api/v1/async_ai_result.
 */
class ProcessWithAIAsyncHandler : public Poco::Net::HTTPRequestHandler {
public:
    void handleRequest(Poco::Net::HTTPServerRequest& request,
                       Poco::Net::HTTPServerResponse& response) override {
        Poco::Timestamp start;
        if (g_httpRequests) g_httpRequests->inc();

        response.setContentType("application/json");
        auto& logger = Poco::Logger::get("Server");

        if (request.getMethod() != "POST") {
            response.setStatus(Poco::Net::HTTPResponse::HTTP_METHOD_NOT_ALLOWED);
            std::ostream& ostr = response.send();
            ostr << R"({"error":"Method not allowed"})";
            if (g_httpErrors) g_httpErrors->inc();
            logDuration(start, request, 405, logger);
            return;
        }

        if (request.getContentType().find("application/json") == std::string::npos) {
            response.setStatus(Poco::Net::HTTPResponse::HTTP_BAD_REQUEST);
            std::ostream& ostr = response.send();
            ostr << R"({"error":"Content-Type must be application/json"})";
            if (g_httpErrors) g_httpErrors->inc();
            logDuration(start, request, 400, logger);
            return;
        }

        try {
            std::string bodyStr;
            Poco::StreamCopier::copyToString(request.stream(), bodyStr);

            if (bodyStr.empty()) {
                response.setStatus(Poco::Net::HTTPResponse::HTTP_BAD_REQUEST);
                std::ostream& ostr = response.send();
                ostr << R"({"error":"Request body is empty"})";
                if (g_httpErrors) g_httpErrors->inc();
                logDuration(start, request, 400, logger);
                return;
            }

            Poco::JSON::Parser parser;
            auto result = parser.parse(bodyStr);
            auto obj = result.extract<Poco::JSON::Object::Ptr>();

            std::string userText;
            if (obj->has("text")) {
                userText = obj->getValue<std::string>("text");
            } else if (obj->has("prompt")) {
                userText = obj->getValue<std::string>("prompt");
            } else if (obj->has("message")) {
                userText = obj->getValue<std::string>("message");
            } else {
                response.setStatus(Poco::Net::HTTPResponse::HTTP_BAD_REQUEST);
                std::ostream& ostr = response.send();
                ostr << R"({"error":"Request must contain 'text', 'prompt' or 'message' field"})";
                if (g_httpErrors) g_httpErrors->inc();
                logDuration(start, request, 400, logger);
                return;
            }

            if (userText.empty()) {
                response.setStatus(Poco::Net::HTTPResponse::HTTP_BAD_REQUEST);
                std::ostream& ostr = response.send();
                ostr << R"({"error":"Text field cannot be empty"})";
                if (g_httpErrors) g_httpErrors->inc();
                logDuration(start, request, 400, logger);
                return;
            }

            if (Poco::Environment::get("OPENAI_API_KEY", "").empty()) {
                response.setStatus(Poco::Net::HTTPResponse::HTTP_SERVICE_UNAVAILABLE);
                std::ostream& ostr = response.send();
                ostr << R"({"error":"AI service not configured. OPENAI_API_KEY is required."})";
                if (g_httpErrors) g_httpErrors->inc();
                logDuration(start, request, 503, logger);
                return;
            }

            int64_t id = openai::AsyncOpenAIManager::instance().submit(userText);

            Poco::JSON::Object jsonResponse;
            jsonResponse.set("request_id", id);
            jsonResponse.set("status", "running");

            response.setStatus(Poco::Net::HTTPResponse::HTTP_ACCEPTED);
            std::ostream& ostr = response.send();
            Poco::JSON::Stringifier::stringify(jsonResponse, ostr);

            logDuration(start, request, 202, logger);

        } catch (const Poco::Exception& e) {
            response.setStatus(Poco::Net::HTTPResponse::HTTP_BAD_REQUEST);
            std::ostream& ostr = response.send();
            ostr << R"({"error":"Invalid JSON: )" << escapeJson(e.displayText()) << "\"}";
            if (g_httpErrors) g_httpErrors->inc();
            logger.error("process_with_ai_async parse error: %s", e.displayText());
            logDuration(start, request, 400, logger);

        } catch (const std::exception& e) {
            response.setStatus(Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR);
            std::ostream& ostr = response.send();
            ostr << R"({"error":"Internal error"})";
            if (g_httpErrors) g_httpErrors->inc();
            logger.error("process_with_ai_async: %s", e.what());
            logDuration(start, request, 500, logger);
        }
    }

private:
    static std::string escapeJson(const std::string& s) {
        std::string result;
        for (char c : s) {
            if (c == '"') result += "\\\"";
            else if (c == '\\') result += "\\\\";
            else if (c == '\n') result += "\\n";
            else if (c == '\r') result += "\\r";
            else if (c == '\t') result += "\\t";
            else result += c;
        }
        return result;
    }

    static void logDuration(Poco::Timestamp start, Poco::Net::HTTPServerRequest& request,
                           int status, Poco::Logger& logger) {
        Poco::Timespan duration = Poco::Timestamp() - start;
        double seconds = static_cast<double>(duration.totalMicroseconds()) / 1000000.0;
        if (g_httpDuration) g_httpDuration->observe(seconds);
        logger.information("%d POST /api/v1/process_with_ai_async from %s, %.2f ms",
                           status, request.clientAddress().toString(), seconds * 1000.0);
    }
};

} // namespace handlers
