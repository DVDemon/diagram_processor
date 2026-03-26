#pragma once

#include <Poco/Net/HTTPRequestHandler.h>
#include <Poco/Net/HTTPServerRequest.h>
#include <Poco/Net/HTTPServerResponse.h>
#include <Poco/URI.h>
#include <Poco/Logger.h>
#include <Poco/Timestamp.h>
#include <Poco/JSON/Array.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>
#include <Poco/JSON/Stringifier.h>

#include "request_counter.h"
#include "../confluence/confluence_client.h"

#include <sstream>
#include <string>

namespace handlers {

namespace {

Poco::JSON::Object::Ptr pageTreeToJson(const confluence::LoadedConfluencePage& node) {
    Poco::JSON::Object::Ptr o = new Poco::JSON::Object();
    o->set("page_id", node.pageId);
    if (node.circularSkip) {
        o->set("error", "circular_reference_in_page_hierarchy");
        o->set("children", Poco::JSON::Array::Ptr(new Poco::JSON::Array()));
        return o;
    }
    std::string html = confluence::ConfluenceClient::storageHtmlFromPageJson(node.pageJson);
    o->set("html", html);
    try {
        Poco::JSON::Parser parser;
        o->set("page", parser.parse(node.pageJson).extract<Poco::JSON::Object::Ptr>());
    } catch (...) {
        o->set("page_raw", node.pageJson);
    }
    Poco::JSON::Array::Ptr children = new Poco::JSON::Array();
    for (const auto& ch : node.children) {
        children->add(pageTreeToJson(*ch));
    }
    o->set("children", children);
    return o;
}

} // namespace

class LoadConfluenceHandler : public Poco::Net::HTTPRequestHandler {
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
            bool useIncludes = (includeSubpages == "1" || includeSubpages == "true");
            // include_subpages: resolve include macros, then recursively load direct child pages (same for each child).
            Poco::JSON::Object::Ptr rootPtr;
            if (useIncludes) {
                confluence::LoadedConfluencePage tree = client.loadPageSubtree(pageId, true);
                std::string html = confluence::ConfluenceClient::storageHtmlFromPageJson(tree.pageJson);
                if (html.empty()) {
                    response.setStatus(Poco::Net::HTTPResponse::HTTP_BAD_GATEWAY);
                    std::ostream& ostr = response.send();
                    ostr << R"({"error":"Failed to load page content"})";
                    if (g_httpErrors) g_httpErrors->inc();
                    logDuration(start, request, 502, logger);
                    return;
                }
                rootPtr = pageTreeToJson(tree);
            } else {
                std::string pageJson = client.getPageById(pageId);
                std::string html = confluence::ConfluenceClient::storageHtmlFromPageJson(pageJson);
                if (html.empty()) {
                    response.setStatus(Poco::Net::HTTPResponse::HTTP_BAD_GATEWAY);
                    std::ostream& ostr = response.send();
                    ostr << R"({"error":"Failed to load page content"})";
                    if (g_httpErrors) g_httpErrors->inc();
                    logDuration(start, request, 502, logger);
                    return;
                }
                rootPtr = new Poco::JSON::Object();
                rootPtr->set("page_id", pageId);
                rootPtr->set("html", html);
                try {
                    Poco::JSON::Parser parser;
                    rootPtr->set("page", parser.parse(pageJson).extract<Poco::JSON::Object::Ptr>());
                } catch (...) {
                    rootPtr->set("page_raw", pageJson);
                }
                rootPtr->set("children", Poco::JSON::Array::Ptr(new Poco::JSON::Array()));
            }

            std::stringstream ss;
            Poco::JSON::Stringifier::stringify(*rootPtr, ss);
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
            logger.error("load_confluence: %s", errMsg.c_str());
            logDuration(start, request, response.getStatus(), logger);

        } catch (const std::exception& e) {
            response.setStatus(Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR);
            std::ostream& ostr = response.send();
            ostr << R"({"error":"Internal error"})";
            if (g_httpErrors) g_httpErrors->inc();
            logger.error("load_confluence: %s", e.what());
            logDuration(start, request, 500, logger);
        }
    }

private:
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
        logger.information("%d GET /api/v1/load_confluence from %s, %.2f ms",
                           status, request.clientAddress().toString(), seconds * 1000.0);
    }
};

} // namespace handlers
