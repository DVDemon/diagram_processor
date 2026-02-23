#pragma once

#include <Poco/Net/HTTPRequestHandler.h>
#include <Poco/Net/HTTPServerRequest.h>
#include <Poco/Net/HTTPServerResponse.h>
#include <Poco/URI.h>
#include <Poco/Logger.h>
#include <Poco/Timestamp.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Array.h>
#include <Poco/JSON/Stringifier.h>

#include "request_counter.h"
#include "../confluence/confluence_client.h"
#include "../confluence/confluence_parser.h"

#include <set>
#include <sstream>
#include <string>

namespace handlers {

class ParseConfluenceHandler : public Poco::Net::HTTPRequestHandler {
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
            std::string pageId = getQueryParam(uri, "page_id");
            std::string includeSubpages = getQueryParam(uri, "include_subpages");

            if (pageId.empty()) {
                response.setStatus(Poco::Net::HTTPResponse::HTTP_BAD_REQUEST);
                std::ostream& ostr = response.send();
                ostr << R"({"error":"page_id query parameter is required"})";
                if (g_httpErrors) g_httpErrors->inc();
                logDuration(start, request, 400, logger);
                return;
            }

            confluence::ConfluenceClient client;
            std::string html = (includeSubpages == "1" || includeSubpages == "true")
                ? client.getPageBodyWithIncludes(pageId)
                : client.getPageBodyById(pageId);

            if (html.empty()) {
                response.setStatus(Poco::Net::HTTPResponse::HTTP_BAD_GATEWAY);
                std::ostream& ostr = response.send();
                ostr << R"({"error":"Failed to load page content"})";
                if (g_httpErrors) g_httpErrors->inc();
                logDuration(start, request, 502, logger);
                return;
            }

            auto diagrams = confluence::ConfluenceParser::parse(html);
            bool includeChildPages = (includeSubpages == "1" || includeSubpages == "true");
            std::set<std::string> resolvedNames;

            for (auto& d : diagrams) {
                if (d.format == confluence::DiagramFormat::DrawIO && d.subtype == "attachment" &&
                    d.text.size() > 15 && d.text.substr(0, 15) == "<!-- DrawIO: ") {
                    size_t end = d.text.find(" (attachment)");
                    if (end != std::string::npos) {
                        std::string diagramName = d.text.substr(15, end - 15);
                        std::string xml = client.getDrawioXmlFromAttachment(pageId, diagramName, includeChildPages);
                        if (!xml.empty()) {
                            d.text = xml;
                            d.subtype = confluence::ConfluenceParser::detectDrawIOTypePublic(xml);
                            resolvedNames.insert(normalizeDiagramName(diagramName));
                        }
                    }
                }
            }

            auto discovered = client.getDrawioAttachmentsByContent(pageId, includeChildPages);
            for (const auto& p : discovered) {
                if (resolvedNames.count(normalizeDiagramName(p.first)) == 0) {
                    confluence::Diagram d;
                    d.text = p.second;
                    d.format = confluence::DiagramFormat::DrawIO;
                    d.subtype = confluence::ConfluenceParser::detectDrawIOTypePublic(p.second);
                    d.sectionTitle = p.first;
                    diagrams.push_back(d);
                }
            }

            Poco::JSON::Object root;
            Poco::JSON::Array arr;
            for (const auto& d : diagrams) {
                Poco::JSON::Object obj;
                obj.set("text", d.text);
                obj.set("format", d.format == confluence::DiagramFormat::PlantUML ? "plantuml" : "drawio");
                obj.set("subtype", d.subtype);
                obj.set("sectionTitle", d.sectionTitle);
                arr.add(obj);
            }
            root.set("diagrams", arr);
            root.set("count", static_cast<int>(diagrams.size()));

            std::stringstream ss;
            Poco::JSON::Stringifier::stringify(root, ss);
            std::string responseBody = ss.str();

            response.setStatus(Poco::Net::HTTPResponse::HTTP_OK);
            std::ostream& ostr = response.send();
            ostr << responseBody;

            logDuration(start, request, 200, logger);

        } catch (const std::runtime_error& e) {
            std::string errMsg = e.what();
            if (errMsg.find("CONFLUENCE") != std::string::npos) {
                response.setStatus(Poco::Net::HTTPResponse::HTTP_SERVICE_UNAVAILABLE);
                std::ostream& ostr = response.send();
                ostr << R"({"error":"Confluence service not configured"})";
            } else {
                response.setStatus(Poco::Net::HTTPResponse::HTTP_BAD_GATEWAY);
                std::ostream& ostr = response.send();
                ostr << R"({"error":")" << escapeJson(errMsg) << "\"}";
            }
            if (g_httpErrors) g_httpErrors->inc();
            logger.error("parse_confluence: %s", errMsg.c_str());
            logDuration(start, request, response.getStatus(), logger);

        } catch (const std::exception& e) {
            response.setStatus(Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR);
            std::ostream& ostr = response.send();
            ostr << R"({"error":"Internal error"})";
            if (g_httpErrors) g_httpErrors->inc();
            logger.error("parse_confluence: %s", e.what());
            logDuration(start, request, 500, logger);
        }
    }

private:
    static std::string normalizeDiagramName(std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (s.size() >= 7 && s.substr(s.size() - 7) == ".drawio") s = s.substr(0, s.size() - 7);
        else if (s.size() >= 4 && s.substr(s.size() - 4) == ".xml") s = s.substr(0, s.size() - 4);
        return s;
    }

    static std::string getQueryParam(const Poco::URI& uri, const std::string& name) {
        for (const auto& p : uri.getQueryParameters()) {
            if (p.first == name) return p.second;
        }
        return "";
    }

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
        logger.information("%d GET /api/v1/parse_confluence from %s, %.2f ms",
                           status, request.clientAddress().toString(), seconds * 1000.0);
    }
};

} // namespace handlers
