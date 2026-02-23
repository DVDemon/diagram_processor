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
#include "../openai/openai_client.h"

#include <sstream>

namespace handlers {

class ProcessWithAIHandler : public Poco::Net::HTTPRequestHandler {
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

            std::string systemPrompt = Poco::Environment::get("OPENAI_SYSTEM_PROMPT", "");
            openai::OpenAIClient client;
            std::string aiResponse = client.chatCompletion(userText, systemPrompt);

            Poco::JSON::Object jsonResponse;
            jsonResponse.set("result", aiResponse);
            jsonResponse.set("success", true);

            response.setStatus(Poco::Net::HTTPResponse::HTTP_OK);
            std::ostream& ostr = response.send();
            Poco::JSON::Stringifier::stringify(jsonResponse, ostr);

            logDuration(start, request, 200, logger);

        } catch (const std::runtime_error& e) {
            std::string errMsg = e.what();
            if (errMsg.find("OPENAI_API_KEY") != std::string::npos) {
                response.setStatus(Poco::Net::HTTPResponse::HTTP_SERVICE_UNAVAILABLE);
                std::ostream& ostr = response.send();
                ostr << R"({"error":"AI service not configured. OPENAI_API_KEY is required."})";
            } else {
                response.setStatus(Poco::Net::HTTPResponse::HTTP_BAD_GATEWAY);
                Poco::JSON::Object errJson;
                errJson.set("error", errMsg);
                errJson.set("success", false);
                std::ostream& ostr = response.send();
                Poco::JSON::Stringifier::stringify(errJson, ostr);
            }
            if (g_httpErrors) g_httpErrors->inc();
            logger.error("process_with_ai: %s", errMsg);
            logDuration(start, request, response.getStatus(), logger);

        } catch (const Poco::Exception& e) {
            response.setStatus(Poco::Net::HTTPResponse::HTTP_BAD_REQUEST);
            Poco::JSON::Object errJson;
            errJson.set("error", "Invalid JSON: " + e.displayText());
            errJson.set("success", false);
            std::ostream& ostr = response.send();
            Poco::JSON::Stringifier::stringify(errJson, ostr);
            if (g_httpErrors) g_httpErrors->inc();
            logger.error("process_with_ai parse error: %s", e.displayText());
            logDuration(start, request, 400, logger);

        } catch (const std::exception& e) {
            response.setStatus(Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR);
            Poco::JSON::Object errJson;
            errJson.set("error", "Internal error");
            errJson.set("success", false);
            std::ostream& ostr = response.send();
            Poco::JSON::Stringifier::stringify(errJson, ostr);
            if (g_httpErrors) g_httpErrors->inc();
            logger.error("process_with_ai: %s", e.what());
            logDuration(start, request, 500, logger);
        }
    }

private:
    static void logDuration(Poco::Timestamp start, Poco::Net::HTTPServerRequest& request,
                           int status, Poco::Logger& logger) {
        Poco::Timespan duration = Poco::Timestamp() - start;
        double seconds = static_cast<double>(duration.totalMicroseconds()) / 1000000.0;
        if (g_httpDuration) g_httpDuration->observe(seconds);
        logger.information("%d POST /api/v1/process_with_ai from %s, %.2f ms",
                           status, request.clientAddress().toString(), seconds * 1000.0);
    }
};

} // namespace handlers
