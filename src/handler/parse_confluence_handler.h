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
#include <Poco/Environment.h>
#include <Poco/NumberParser.h>

#include "request_counter.h"
#include "../confluence/confluence_client.h"
#include "../confluence/confluence_parser.h"

#include <set>
#include <algorithm>
#include <cctype>
#include <sstream>
#include <functional>
#include <string>
#include <utility>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <deque>
#include <cstdlib>

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
            bool useIncludes = (includeSubpages == "1" || includeSubpages == "true");
            std::vector<std::pair<std::string, confluence::Diagram>> diagramRows;
            std::set<std::string> resolvedKeys;
            // Global deduplication to ensure no duplicate diagrams are returned
            // even if they come from multiple macro/embed/plugin variants or multiple pages.
            std::unordered_set<size_t> emittedDiagramHashes;
            ParseProgress progress;

            if (useIncludes) {
                auto pagesWithDepth = collectPageIdsWithDepth(client, pageId, logger, progress);

                bool hasAnyHtml = false;
                for (const auto& p : pagesWithDepth) {
                    const std::string& pid = p.first;
                    const size_t depth = p.second;
                    try {
                        std::string html = client.getPageBodyWithIncludes(pid);
                        progress.resourcesLoaded++;
                        logger.information(
                            "parse_confluence progress: depth=" + std::to_string(depth) +
                            " resources_loaded=" + std::to_string(progress.resourcesLoaded) +
                            " pages_processed=" + std::to_string(progress.pagesProcessed) +
                            " stage=page_body page_id=" + pid);
                        if (html.empty()) continue;
                        if (html.size() > maxParsePageBytes()) {
                            logger.warning(
                                "parse_confluence skip page: page_id=" + pid +
                                " depth=" + std::to_string(depth) +
                                " reason=page_body_too_large bytes=" + std::to_string(html.size()));
                            continue;
                        }
                        hasAnyHtml = true;
                        appendDiagramsForPage(pid, html, client, diagramRows, resolvedKeys, emittedDiagramHashes, false);
                        progress.pagesProcessed++;
                        logger.information(
                            "parse_confluence progress: depth=" + std::to_string(depth) +
                            " resources_loaded=" + std::to_string(progress.resourcesLoaded) +
                            " pages_processed=" + std::to_string(progress.pagesProcessed) +
                            " stage=page_parsed page_id=" + pid);
                    } catch (...) {
                        // Skip failed page and continue with the rest.
                    }
                }
                if (!hasAnyHtml) {
                    response.setStatus(Poco::Net::HTTPResponse::HTTP_BAD_GATEWAY);
                    std::ostream& ostr = response.send();
                    ostr << R"({"error":"Failed to load page content"})";
                    if (g_httpErrors) g_httpErrors->inc();
                    logDuration(start, request, 502, logger);
                    return;
                }
            } else {
                std::string html = client.getPageBodyById(pageId);
                progress.resourcesLoaded++;
                logger.information(
                    "parse_confluence progress: depth=0 resources_loaded=" +
                    std::to_string(progress.resourcesLoaded) +
                    " pages_processed=" + std::to_string(progress.pagesProcessed) +
                    " stage=page_body page_id=" + pageId);
                if (html.empty()) {
                    response.setStatus(Poco::Net::HTTPResponse::HTTP_BAD_GATEWAY);
                    std::ostream& ostr = response.send();
                    ostr << R"({"error":"Failed to load page content"})";
                    if (g_httpErrors) g_httpErrors->inc();
                    logDuration(start, request, 502, logger);
                    return;
                }
                if (html.size() > maxParsePageBytes()) {
                    response.setStatus(Poco::Net::HTTPResponse::HTTP_BAD_GATEWAY);
                    std::ostream& ostr = response.send();
                    ostr << R"({"error":"Page body too large for parsing"})";
                    if (g_httpErrors) g_httpErrors->inc();
                    logger.warning(
                        "parse_confluence skip root page: page_id=" + pageId +
                        " reason=page_body_too_large bytes=" + std::to_string(html.size()));
                    logDuration(start, request, 502, logger);
                    return;
                }
                appendDiagramsForPage(pageId, html, client, diagramRows, resolvedKeys, emittedDiagramHashes, false);
                progress.pagesProcessed++;
                logger.information(
                    "parse_confluence progress: depth=0 resources_loaded=" +
                    std::to_string(progress.resourcesLoaded) +
                    " pages_processed=" + std::to_string(progress.pagesProcessed) +
                    " stage=page_parsed page_id=" + pageId);
            }

            Poco::JSON::Object root;
            Poco::JSON::Array arr;
            for (const auto& row : diagramRows) {
                const confluence::Diagram& d = row.second;
                Poco::JSON::Object obj;
                obj.set("text", d.text);
                obj.set("format", d.format == confluence::DiagramFormat::PlantUML ? "plantuml" : "drawio");
                obj.set("subtype", d.subtype);
                obj.set("sectionTitle", d.sectionTitle);
                obj.set("source_page_id", row.first);
                arr.add(obj);
            }
            root.set("diagrams", arr);
            root.set("count", static_cast<int>(diagramRows.size()));

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
            logger.error("parse_confluence: " + errMsg);
            logDuration(start, request, response.getStatus(), logger);

        } catch (const std::exception& e) {
            response.setStatus(Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR);
            std::ostream& ostr = response.send();
            ostr << R"({"error":"Internal error"})";
            if (g_httpErrors) g_httpErrors->inc();
            logger.error("parse_confluence: " + std::string(e.what()));
            logDuration(start, request, 500, logger);
        }
    }

private:
    struct ParseProgress {
        size_t resourcesLoaded = 0;
        size_t pagesProcessed = 0;
    };

    static size_t maxParsePageBytes() {
        try {
            int v = Poco::NumberParser::parse(Poco::Environment::get("CONFLUENCE_PARSE_PAGE_MAX_BYTES", "4194304"));
            return v > 0 ? static_cast<size_t>(v) : 4194304u;
        } catch (...) {
            return 4194304u;
        }
    }

    static std::vector<std::pair<std::string, size_t>> collectPageIdsWithDepth(
        confluence::ConfluenceClient& client,
        const std::string& rootPageId,
        Poco::Logger& logger,
        ParseProgress& progress) {
        std::vector<std::pair<std::string, size_t>> ordered;
        std::deque<std::pair<std::string, size_t>> q;
        std::unordered_set<std::string> seen;
        q.emplace_back(rootPageId, 0);
        seen.insert(rootPageId);

        while (!q.empty()) {
            auto cur = q.front();
            q.pop_front();
            const std::string& pid = cur.first;
            const size_t depth = cur.second;
            ordered.push_back(cur);

            try {
                std::string childrenJson = client.getChildPages(pid);
                progress.resourcesLoaded++;
                logger.information(
                    "parse_confluence progress: depth=" + std::to_string(depth) +
                    " resources_loaded=" + std::to_string(progress.resourcesLoaded) +
                    " pages_processed=" + std::to_string(progress.pagesProcessed) +
                    " stage=child_list page_id=" + pid);
                auto childIds = confluence::ConfluenceClient::extractChildPageIds(childrenJson);
                for (const auto& cid : childIds) {
                    if (seen.insert(cid).second) q.emplace_back(cid, depth + 1);
                }
            } catch (...) {}
        }
        return ordered;
    }

    static size_t diagramHash(const confluence::Diagram& d) {
        // Collision risk exists for hashes, but this is acceptable for "best-effort" dedup.
        size_t h = std::hash<std::string>{}(d.text);
        // Mix format into the key to avoid collisions between PlantUML and DrawIO with same text.
        h ^= (d.format == confluence::DiagramFormat::DrawIO)
                 ? static_cast<size_t>(0x9e3779b97f4a7c15ULL)
                 : static_cast<size_t>(0x243f6a8885a308d3ULL);
        return h;
    }

    static std::string toLowerCopy(std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    static std::string detectPlantUmlTypeInHandler(const std::string& code) {
        std::string lower = toLowerCopy(code);
        int seq = 0, comp = 0, c4 = 0, usecase = 0;
        if (lower.find("participant") != std::string::npos) seq += 3;
        if (lower.find("activate") != std::string::npos) seq++;
        if (lower.find("->") != std::string::npos || lower.find("-->") != std::string::npos) seq++;
        if (lower.find("component") != std::string::npos) comp += 2;
        if (lower.find("interface") != std::string::npos) comp++;
        if (lower.find("person") != std::string::npos) c4++;
        if (lower.find("system") != std::string::npos) c4++;
        if (lower.find("container") != std::string::npos) c4++;
        if (lower.find("usecase") != std::string::npos) usecase += 3;
        if (lower.find("actor") != std::string::npos) usecase++;
        if (lower.find("extends") != std::string::npos || lower.find("includes") != std::string::npos) usecase++;

        int maxVal = std::max({seq, comp, c4, usecase});
        if (maxVal == 0) return "unknown";
        if (seq == maxVal) return "sequence";
        if (comp == maxVal) return "component";
        if (c4 == maxVal) return "c4";
        if (usecase == maxVal) return "usecase";
        return "unknown";
    }

    static std::vector<std::string> extractInlinePlantUmlCodeFromHtml(const std::string& html) {
        std::vector<std::string> out;
        size_t pos = 0;
        while (out.size() < 200) {
            size_t start = html.find("@startuml", pos);
            if (start == std::string::npos) break;
            size_t end = html.find("@enduml", start);
            if (end == std::string::npos) break;
            end += std::string("@enduml").size();
            out.push_back(html.substr(start, end - start));
            pos = end;
        }
        return out;
    }

    static std::vector<std::string> extractInlineDrawioXmlFromHtml(const std::string& html) {
        std::vector<std::string> out;
        size_t pos = 0;
        while (out.size() < 200) {
            size_t start = html.find("<mxfile", pos);
            if (start == std::string::npos) break;
            size_t end = html.find("</mxfile>", start);
            if (end == std::string::npos) break;
            end += std::string("</mxfile>").size();
            out.push_back(html.substr(start, end - start));
            pos = end;
        }

        pos = 0;
        while (out.size() < 200) {
            size_t start = html.find("<mxGraphModel", pos);
            if (start == std::string::npos) break;
            size_t end = html.find("</mxGraphModel>", start);
            if (end == std::string::npos) break;
            end += std::string("</mxGraphModel>").size();
            out.push_back(html.substr(start, end - start));
            pos = end;
        }
        return out;
    }

    static bool regexParsingEnabled() {
        std::string v = Poco::Environment::get("CONFLUENCE_REGEX_PARSING_ENABLED", "false");
        return v == "1" || v == "true" || v == "TRUE";
    }

    static void appendDiagramsForPage(const std::string& sourcePageId,
                                      const std::string& html,
                                      confluence::ConfluenceClient& client,
                                      std::vector<std::pair<std::string, confluence::Diagram>>& out,
                                      std::set<std::string>& resolvedKeys,
                                      std::unordered_set<size_t>& emittedDiagramHashes,
                                      bool searchDescendantsForAttachments) {
        std::vector<confluence::Diagram> diagrams;
        if (regexParsingEnabled()) {
            diagrams = confluence::ConfluenceParser::parse(html);
        }

        for (auto& d : diagrams) {
            if (d.format == confluence::DiagramFormat::DrawIO && d.subtype == "attachment" &&
                d.text.size() > 15 && d.text.substr(0, 15) == "<!-- DrawIO: ") {
                size_t end = d.text.find(" (attachment)");
                if (end != std::string::npos) {
                    std::string diagramName = d.text.substr(15, end - 15);
                    std::string xml = client.getDrawioXmlFromAttachment(sourcePageId, diagramName,
                                                                        searchDescendantsForAttachments);
                    if (!xml.empty()) {
                        d.text = xml;
                        d.subtype = confluence::ConfluenceParser::detectDrawIOTypePublic(xml);
                        resolvedKeys.insert(sourcePageId + "|" + normalizeDiagramName(diagramName));
                    }
                }
            }
        }

        // Safe default path: detect DrawIO XML directly in storage HTML.
        std::unordered_set<size_t> existingInlineHashes;
        for (const auto& d : diagrams) {
            if (d.format != confluence::DiagramFormat::DrawIO) continue;
            if (d.text.find("<mxfile") != std::string::npos ||
                d.text.find("<mxGraphModel") != std::string::npos) {
                existingInlineHashes.insert(std::hash<std::string>{}(d.text));
            }
        }
        auto inlineXmls = extractInlineDrawioXmlFromHtml(html);
        for (const auto& xml : inlineXmls) {
            size_t h = std::hash<std::string>{}(xml);
            if (existingInlineHashes.count(h) == 0) {
                confluence::Diagram d;
                d.text = xml;
                d.format = confluence::DiagramFormat::DrawIO;
                d.subtype = confluence::ConfluenceParser::detectDrawIOTypePublic(xml);
                d.sectionTitle.clear();
                diagrams.push_back(std::move(d));
                existingInlineHashes.insert(h);
            }
        }

        // Safe default path: detect PlantUML code directly in storage HTML.
        std::unordered_set<size_t> existingPlantHashes;
        for (const auto& d : diagrams) {
            if (d.format != confluence::DiagramFormat::PlantUML) continue;
            if (d.text.find("@startuml") == std::string::npos &&
                d.text.find("@enduml") == std::string::npos) {
                continue;
            }
            existingPlantHashes.insert(std::hash<std::string>{}(d.text));
        }

        auto plantCodes = extractInlinePlantUmlCodeFromHtml(html);
        for (const auto& code : plantCodes) {
            size_t h = std::hash<std::string>{}(code);
            if (existingPlantHashes.count(h) != 0) continue;
            confluence::Diagram d;
            d.text = code;
            d.format = confluence::DiagramFormat::PlantUML;
            d.subtype = detectPlantUmlTypeInHandler(code);
            d.sectionTitle.clear();
            if (emittedDiagramHashes.insert(diagramHash(d)).second) {
                out.emplace_back(sourcePageId, std::move(d));
            }
            existingPlantHashes.insert(h);
        }

        for (auto& d : diagrams) {
            size_t h = diagramHash(d);
            if (emittedDiagramHashes.insert(h).second) {
                out.emplace_back(sourcePageId, std::move(d));
            }
        }

        auto discovered = client.getDrawioAttachmentsByContent(sourcePageId, searchDescendantsForAttachments);
        for (const auto& p : discovered) {
            std::string key = sourcePageId + "|" + normalizeDiagramName(p.first);
            if (resolvedKeys.count(key) == 0) {
                confluence::Diagram d;
                d.text = p.second;
                d.format = confluence::DiagramFormat::DrawIO;
                d.subtype = confluence::ConfluenceParser::detectDrawIOTypePublic(p.second);
                d.sectionTitle = p.first;
                if (emittedDiagramHashes.insert(diagramHash(d)).second) {
                    out.emplace_back(sourcePageId, std::move(d));
                }
                resolvedKeys.insert(std::move(key));
            }
        }
    }

    static void appendDiagramsFromPageTree(const confluence::LoadedConfluencePage& node,
                                           confluence::ConfluenceClient& client,
                                           std::vector<std::pair<std::string, confluence::Diagram>>& out,
                                           std::set<std::string>& resolvedKeys,
                                           std::unordered_set<size_t>& emittedDiagramHashes) {
        // Iterative traversal to avoid stack overflows on deep page trees.
        std::deque<const confluence::LoadedConfluencePage*> queue;
        queue.push_back(&node);
        while (!queue.empty()) {
            const confluence::LoadedConfluencePage* cur = queue.front();
            queue.pop_front();
            if (!cur || cur->circularSkip) {
                continue;
            }
            std::string html = confluence::ConfluenceClient::storageHtmlFromPageJson(cur->pageJson);
            if (!html.empty()) {
                appendDiagramsForPage(cur->pageId, html, client, out, resolvedKeys, emittedDiagramHashes, false);
            }
            for (const auto& ch : cur->children) {
                if (ch) queue.push_back(ch.get());
            }
        }
    }

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
