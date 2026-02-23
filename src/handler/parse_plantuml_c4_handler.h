#pragma once

#include <Poco/Net/HTTPRequestHandler.h>
#include <Poco/Net/HTTPServerRequest.h>
#include <Poco/Net/HTTPServerResponse.h>
#include <Poco/Logger.h>
#include <Poco/Timestamp.h>
#include <Poco/JSON/Parser.h>
#include <Poco/JSON/Object.h>
#include <Poco/StreamCopier.h>

#include "request_counter.h"
#include "../plantuml/plantuml_c4.h"

#include <sstream>
#include <string>

namespace handlers {

class ParsePlantumlC4Handler : public Poco::Net::HTTPRequestHandler {
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
            ostr << R"({"error":"Method not allowed","success":false})";
            if (g_httpErrors) g_httpErrors->inc();
            logDuration(start, request, 405, logger);
            return;
        }

        try {
            std::string bodyStr;
            Poco::StreamCopier::copyToString(request.stream(), bodyStr);

            std::string plantumlText;
            std::string contentType = request.getContentType();
            if (contentType.find("application/json") != std::string::npos && !bodyStr.empty()) {
                Poco::JSON::Parser parser;
                auto result = parser.parse(bodyStr);
                auto obj = result.extract<Poco::JSON::Object::Ptr>();
                if (obj->has("text")) {
                    plantumlText = obj->getValue<std::string>("text");
                } else if (obj->has("plantuml")) {
                    plantumlText = obj->getValue<std::string>("plantuml");
                } else if (obj->has("content")) {
                    plantumlText = obj->getValue<std::string>("content");
                } else {
                    response.setStatus(Poco::Net::HTTPResponse::HTTP_BAD_REQUEST);
                    std::ostream& ostr = response.send();
                    ostr << R"({"error":"Request must contain 'text', 'plantuml' or 'content' field","success":false})";
                    if (g_httpErrors) g_httpErrors->inc();
                    logDuration(start, request, 400, logger);
                    return;
                }
            } else {
                plantumlText = bodyStr;
            }

            if (plantumlText.empty()) {
                response.setStatus(Poco::Net::HTTPResponse::HTTP_BAD_REQUEST);
                std::ostream& ostr = response.send();
                ostr << R"({"error":"PlantUML text cannot be empty","success":false})";
                if (g_httpErrors) g_httpErrors->inc();
                logDuration(start, request, 400, logger);
                return;
            }

            std::string jsonResult = plantuml::PlantUmlC4Parser::parse(plantumlText);

            Poco::JSON::Parser parser;
            auto parsed = parser.parse(jsonResult);
            auto obj = parsed.extract<Poco::JSON::Object::Ptr>();
            if (obj->has("success") && !obj->getValue<bool>("success")) {
                response.setStatus(Poco::Net::HTTPResponse::HTTP_BAD_REQUEST);
            } else {
                response.setStatus(Poco::Net::HTTPResponse::HTTP_OK);
            }

            std::ostream& ostr = response.send();
            ostr << jsonResult;

            logDuration(start, request, response.getStatus(), logger);

        } catch (const Poco::Exception& e) {
            response.setStatus(Poco::Net::HTTPResponse::HTTP_BAD_REQUEST);
            std::ostream& ostr = response.send();
            ostr << R"({"error":")" << escapeJson("Invalid JSON: " + e.displayText()) << R"(","success":false})";
            if (g_httpErrors) g_httpErrors->inc();
            logger.error("parse_plantuml_c4: %s", e.displayText().c_str());
            logDuration(start, request, 400, logger);

        } catch (const std::exception& e) {
            response.setStatus(Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR);
            std::ostream& ostr = response.send();
            ostr << R"({"error":")" << escapeJson(e.what()) << R"(","success":false})";
            if (g_httpErrors) g_httpErrors->inc();
            logger.error("parse_plantuml_c4: %s", e.what());
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
        logger.information("%d POST /api/v1/parse_plantuml_c4 from %s, %.2f ms",
                           status, request.clientAddress().toString(), seconds * 1000.0);
    }
};

} // namespace handlers
