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
#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>
#include <functional>
#include <string>
#include <utility>
#include <unordered_set>
#include <vector>

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

            if (useIncludes) {
                confluence::LoadedConfluencePage tree = client.loadPageSubtree(pageId, true);
                std::string rootHtml = confluence::ConfluenceClient::storageHtmlFromPageJson(tree.pageJson);
                if (rootHtml.empty()) {
                    response.setStatus(Poco::Net::HTTPResponse::HTTP_BAD_GATEWAY);
                    std::ostream& ostr = response.send();
                    ostr << R"({"error":"Failed to load page content"})";
                    if (g_httpErrors) g_httpErrors->inc();
                    logDuration(start, request, 502, logger);
                    return;
                }
                appendDiagramsFromPageTree(tree, client, diagramRows, resolvedKeys, emittedDiagramHashes);
            } else {
                std::string html = client.getPageBodyById(pageId);
                if (html.empty()) {
                    response.setStatus(Poco::Net::HTTPResponse::HTTP_BAD_GATEWAY);
                    std::ostream& ostr = response.send();
                    ostr << R"({"error":"Failed to load page content"})";
                    if (g_httpErrors) g_httpErrors->inc();
                    logDuration(start, request, 502, logger);
                    return;
                }
                appendDiagramsForPage(pageId, html, client, diagramRows, resolvedKeys, emittedDiagramHashes, false);
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
        // Best-effort: capture PlantUML code that appears as CDATA/plain-text-body even if
        // the surrounding macro name isn't exactly "plantuml".
        std::vector<std::string> out;
        std::regex cdataRe("<!\\[CDATA\\[([\\s\\S]*?@startuml[\\s\\S]*?@enduml[\\s\\S]*?)\\]\\]>",
                           std::regex::icase);
        std::sregex_iterator it1(html.begin(), html.end(), cdataRe);
        std::sregex_iterator end;
        for (; it1 != end; ++it1) {
            out.push_back((*it1)[1].str());
            if (out.size() >= 200) break;
        }

        std::regex ptbRe("<ac:plain-text-body[^>]*>([\\s\\S]*?@startuml[\\s\\S]*?@enduml[\\s\\S]*?)</ac:plain-text-body>",
                          std::regex::icase);
        std::sregex_iterator it2(html.begin(), html.end(), ptbRe);
        for (; it2 != end; ++it2) {
            out.push_back((*it2)[1].str());
            if (out.size() >= 200) break;
        }
        return out;
    }

    static std::vector<std::string> extractInlineDrawioXmlFromHtml(const std::string& html) {
        // Best-effort: plugin/embedded variants may not be wrapped into ac:structured-macro
        // with ac:name="drawio". In on-prem storage HTML the actual diagram XML usually still
        // contains <mxfile> or <mxGraphModel> tags.
        std::vector<std::string> out;

        std::regex mxfileRe("<mxfile[^>]*>[\\s\\S]*?</mxfile>", std::regex::icase);
        std::sregex_iterator it1(html.begin(), html.end(), mxfileRe);
        std::sregex_iterator end;
        for (; it1 != end; ++it1) {
            out.push_back((*it1).str());
            if (out.size() >= 200) break; // safety
        }

        std::regex mxGraphModelRe("<mxGraphModel[^>]*>[\\s\\S]*?</mxGraphModel>", std::regex::icase);
        std::sregex_iterator it2(html.begin(), html.end(), mxGraphModelRe);
        for (; it2 != end; ++it2) {
            std::string xml = (*it2).str();
            // Avoid duplicates like when <mxGraphModel> is inside already captured <mxfile>.
            if (xml.find("<mxfile") == std::string::npos) out.push_back(std::move(xml));
            if (out.size() >= 200) break;
        }
        return out;
    }

    static void appendDiagramsForPage(const std::string& sourcePageId,
                                      const std::string& html,
                                      confluence::ConfluenceClient& client,
                                      std::vector<std::pair<std::string, confluence::Diagram>>& out,
                                      std::set<std::string>& resolvedKeys,
                                      std::unordered_set<size_t>& emittedDiagramHashes,
                                      bool searchDescendantsForAttachments) {
        auto diagrams = confluence::ConfluenceParser::parse(html);

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

        // Fallback for embed/plugin: ConfluenceParser might miss some macro names.
        // If we detect DrawIO XML directly in storage HTML, include it as diagrams too.
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

        for (auto& d : diagrams) {
            size_t h = diagramHash(d);
            if (emittedDiagramHashes.insert(h).second) {
                out.emplace_back(sourcePageId, std::move(d));
            }
        }

        // Fallback for embed/plugin variants: PlantUML code may exist in storage HTML without
        // being wrapped by ac:name="plantuml" exactly. Add it best-effort.
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
        if (!node.circularSkip) {
            std::string html = confluence::ConfluenceClient::storageHtmlFromPageJson(node.pageJson);
            if (!html.empty()) {
                appendDiagramsForPage(node.pageId, html, client, out, resolvedKeys, emittedDiagramHashes, false);
            }
        }
        for (const auto& ch : node.children) {
            appendDiagramsFromPageTree(*ch, client, out, resolvedKeys, emittedDiagramHashes);
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
