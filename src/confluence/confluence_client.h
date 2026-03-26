#pragma once

#include <Poco/Net/HTTPSClientSession.h>
#include <Poco/Net/HTTPRequest.h>
#include <Poco/Net/HTTPResponse.h>
#include <Poco/Net/Context.h>
#include <Poco/URI.h>
#include <Poco/StreamCopier.h>
#include <Poco/Environment.h>
#include <Poco/NumberParser.h>
#include <Poco/Logger.h>
#include <Poco/Base64Encoder.h>
#include <Poco/JSON/Parser.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Stringifier.h>

#include <algorithm>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace confluence {

/** One node of a page tree: page JSON plus direct child pages (same loading rules as root). */
struct LoadedConfluencePage {
    std::string pageId;
    std::string pageJson;
    std::vector<std::unique_ptr<LoadedConfluencePage>> children;
    bool circularSkip = false;
};

/**
 * Confluence REST client. Primary target: on-premises Confluence Server / Data Center (REST API v1
 * under /rest/api). Optional: Confluence Cloud (REST API v2 under /wiki/api/v2) when
 * CONFLUENCE_API_TYPE=cloud.
 *
 * Environment:
 *   CONFLUENCE_URL   - Base URL without trailing slash. On-prem: https://confluence.company.local
 *                      (or with context path, e.g. https://host/confluence). Cloud: https://tenant.atlassian.net
 *   CONFLUENCE_USER  - For Basic auth (username; often still email on Cloud)
 *   CONFLUENCE_TOKEN - Personal Access Token / API token / password (with Basic auth)
 *   CONFLUENCE_API_TYPE - server | onprem | datacenter | dc (default) = on-prem REST v1.
 *                         cloud | v2 = Atlassian Cloud REST v2.
 *   CONFLUENCE_TIMEOUT - Seconds (default: 30)
 *   CONFLUENCE_SSL_VERIFY - true/false (default: true)
 *
 * Auth: CONFLUENCE_USER set -> Basic auth (user:token). Else Bearer CONFLUENCE_TOKEN.
 */
class ConfluenceClient {
public:
    ConfluenceClient()
        : baseUrl_(normalizeUrl(Poco::Environment::get("CONFLUENCE_URL", ""))),
          user_(Poco::Environment::get("CONFLUENCE_USER", "")),
          token_(Poco::Environment::get("CONFLUENCE_TOKEN", "")),
          apiType_(parseApiType(Poco::Environment::get("CONFLUENCE_API_TYPE", "server"))),
          timeoutSec_(parseTimeout(Poco::Environment::get("CONFLUENCE_TIMEOUT", "30"))),
          verifySsl_(parseBool(Poco::Environment::get("CONFLUENCE_SSL_VERIFY", "true"))) {
    }

    /**
     * Get page data by page ID.
     * @param pageId Confluence page ID
     * @return JSON response body as string
     */
    std::string getPageById(const std::string& pageId) const {
        std::string path = apiType_ == ApiType::Cloud
            ? "/wiki/api/v2/pages/" + pageId + "?body-format=storage"
            : "/rest/api/content/" + pageId + "?expand=body.storage";
        return doGet(path);
    }

    /**
     * Get page data by Confluence page URL.
     * Extracts page ID from URL (e.g. .../pages/123456789/... or .../pages/123456789)
     * @param pageUrl Full Confluence page URL
     * @return JSON response body as string
     */
    std::string getPageByUrl(const std::string& pageUrl) const {
        std::string pageId = extractPageIdFromUrl(pageUrl);
        if (pageId.empty()) {
            throw std::runtime_error("Could not extract page ID from URL: " + pageUrl);
        }
        return getPageById(pageId);
    }

    /**
     * Get list of child pages for a page.
     * @param pageId Parent page ID
     * @return JSON response body as string (contains results array)
     */
    std::string getChildPages(const std::string& pageId) const {
        std::string path = apiType_ == ApiType::Cloud
            ? "/wiki/api/v2/pages/" + pageId + "/children?limit=250"
            : "/rest/api/content/" + pageId + "/child/page?limit=100&expand=page";
        return doGet(path);
    }

    /**
     * Get page content with recursively resolved include macros (sub-pages).
     * Finds ac:structured-macro ac:name="include" in Confluence storage HTML,
     * fetches each included page, recursively processes its includes, and replaces
     * the macro with the included content. Protects against circular includes.
     * @param pageId Confluence page ID
     * @return JSON response (same structure as getPageById) with body.storage.value
     *         containing the assembled HTML with all includes resolved
     */
    std::string getPageWithIncludes(const std::string& pageId) const {
        std::string json = getPageById(pageId);
        std::string bodyHtml = extractBodyFromPageJson(json);
        if (bodyHtml.empty()) {
            return json;
        }
        std::set<std::string> processed;
        std::string expanded = processIncludesRecursively(bodyHtml, processed);
        return replaceBodyInPageJson(json, expanded);
    }

    /**
     * Get page body HTML (without includes).
     * @param pageId Confluence page ID
     * @return HTML content string, or empty on error
     */
    std::string getPageBodyById(const std::string& pageId) const {
        std::string json = getPageById(pageId);
        return extractBodyFromPageJson(json);
    }

    /** Storage HTML from a page JSON payload (same shape as getPageById / getPageWithIncludes). */
    static std::string storageHtmlFromPageJson(const std::string& pageJson) {
        return extractBodyFromPageJson(pageJson);
    }

    /**
     * Get page body HTML with resolved includes (for parsing).
     * @param pageId Confluence page ID
     * @return HTML content string, or empty on error
     */
    std::string getPageBodyWithIncludes(const std::string& pageId) const {
        std::string json = getPageWithIncludes(pageId);
        return extractBodyFromPageJson(json);
    }

    /**
     * Load a page and all descendant pages in the hierarchy (direct children, recursively).
     * For each page: same as getPageById / getPageWithIncludes (when resolveIncludes is true).
     * Order: resolve includes on a page, then load each child with the same algorithm.
     * Protects against cycles in the page tree (skipped nodes have circularSkip set).
     */
    LoadedConfluencePage loadPageSubtree(const std::string& pageId, bool resolveIncludes) const {
        std::set<std::string> visited;
        return loadPageSubtreeImpl(pageId, resolveIncludes, visited);
    }

    /**
     * Get all DrawIO diagrams from page attachments (by content: mxfile or mxGraphModel).
     * @param pageId Confluence page ID
     * @param includeChildPages If true, also search in child page attachments
     * @return Vector of (baseFilename, xml) for each DrawIO attachment
     */
    std::vector<std::pair<std::string, std::string>> getDrawioAttachmentsByContent(const std::string& pageId,
                                                                                    bool includeChildPages) const {
        std::vector<std::pair<std::string, std::string>> out;
        collectDrawioFromPage(pageId, out);
        if (includeChildPages) {
            try {
                for (const auto& cid : collectDescendantPageIds(pageId)) {
                    collectDrawioFromPage(cid, out);
                }
            } catch (...) {}
        }
        return out;
    }

    /**
     * Get DrawIO XML from page attachment by diagram name.
     * Uses content-based analysis: fetches each attachment, checks for DrawIO XML, matches by filename.
     * Tries main page and child pages (for include_subpages case).
     * @param pageId Confluence page ID
     * @param diagramName Diagram name from macro (diagramName or diagramDisplayName param)
     * @param includeChildPages If true, also search in child page attachments
     * @return XML content or empty string on failure
     */
    std::string getDrawioXmlFromAttachment(const std::string& pageId, const std::string& diagramName,
                                           bool includeChildPages = false) const {
        std::string xml = tryGetDrawioFromPage(pageId, diagramName);
        if (!xml.empty()) return xml;
        if (includeChildPages) {
            try {
                std::vector<std::string> toSearch = collectDescendantPageIds(pageId);
                for (const auto& cid : toSearch) {
                    xml = tryGetDrawioFromPage(cid, diagramName);
                    if (!xml.empty()) return xml;
                }
            } catch (...) {}
        }
        return "";
    }

    void collectDrawioFromPage(const std::string& pageId,
                               std::vector<std::pair<std::string, std::string>>& out) const {
        try {
            std::string json = doGet("/rest/api/content/" + pageId + "/child/attachment");
            for (const auto& att : extractAllAttachments(json, pageId)) {
                std::string content = doGet(att.second);
                if (content.find("<mxfile") != std::string::npos || content.find("<mxGraphModel") != std::string::npos) {
                    size_t dot = att.first.rfind('.');
                    std::string base = (dot != std::string::npos) ? att.first.substr(0, dot) : att.first;
                    out.emplace_back(base, content);
                }
            }
        } catch (...) {}
    }

    std::string tryGetDrawioFromPage(const std::string& pageId, const std::string& diagramName) const {
        try {
            std::string json = doGet("/rest/api/content/" + pageId + "/child/attachment");
            std::vector<std::pair<std::string, std::string>> attachments = extractAllAttachments(json, pageId);
            std::string nameLower = diagramName;
            for (char& c : nameLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (nameLower.size() >= 7 && nameLower.substr(nameLower.size() - 7) == ".drawio")
                nameLower = nameLower.substr(0, nameLower.size() - 7);
            else if (nameLower.size() >= 4 && nameLower.substr(nameLower.size() - 4) == ".xml")
                nameLower = nameLower.substr(0, nameLower.size() - 4);
            std::string fallbackXml;
            int drawioCount = 0;
            for (const auto& att : attachments) {
                std::string content = doGet(att.second);
                if (content.find("<mxfile") != std::string::npos || content.find("<mxGraphModel") != std::string::npos) {
                    drawioCount++;
                    std::string titleLower = att.first;
                    for (char& c : titleLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    size_t dot = titleLower.rfind('.');
                    std::string baseName = (dot != std::string::npos) ? titleLower.substr(0, dot) : titleLower;
                    bool match = (baseName == nameLower || baseName.find(nameLower) != std::string::npos ||
                                 nameLower.find(baseName) != std::string::npos);
                    if (match) return content;
                    if (fallbackXml.empty()) fallbackXml = content;
                }
            }
            if (!fallbackXml.empty() && drawioCount == 1) return fallbackXml;
            return "";
        } catch (...) {
            return "";
        }
    }

    std::vector<std::string> collectDescendantPageIds(const std::string& pageId) const {
        std::vector<std::string> ids;
        std::set<std::string> seen;
        std::vector<std::string> queue = {pageId};
        while (!queue.empty()) {
            std::string pid = queue.back();
            queue.pop_back();
            if (seen.count(pid)) continue;
            seen.insert(pid);
            try {
                std::vector<std::string> childIds = fetchAllDirectChildPageIds(pid);
                for (const auto& cid : childIds) {
                    if (seen.count(cid) == 0) {
                        ids.push_back(cid);
                        queue.push_back(cid);
                    }
                }
            } catch (...) { break; }
        }
        return ids;
    }

    static std::vector<std::string> extractChildPageIds(const std::string& jsonStr) {
        std::vector<std::string> ids;
        try {
            Poco::JSON::Parser parser;
            auto result = parser.parse(jsonStr);
            auto obj = result.extract<Poco::JSON::Object::Ptr>();
            if (!obj->has("results")) return ids;
            auto results = obj->getArray("results");
            for (size_t i = 0; i < results->size(); ++i) {
                auto item = results->getObject(i);
                std::string id;
                if (item->has("id")) {
                    id = item->getValue<std::string>("id");
                } else if (item->has("page")) {
                    auto page = item->getObject("page");
                    if (page->has("id")) id = page->getValue<std::string>("id");
                } else if (item->has("content")) {
                    auto content = item->getObject("content");
                    if (content->has("id")) id = content->getValue<std::string>("id");
                }
                if (!id.empty()) ids.push_back(id);
            }
        } catch (...) {}
        return ids;
    }

    const std::string& getBaseUrl() const { return baseUrl_; }
    int getTimeoutSec() const { return timeoutSec_; }

private:
    /** Pagination _links.next (Confluence Cloud REST API v2 children responses). */
    static std::string extractCloudChildrenNextPath(const std::string& jsonStr) {
        try {
            Poco::JSON::Parser parser;
            auto obj = parser.parse(jsonStr).extract<Poco::JSON::Object::Ptr>();
            if (!obj->has("_links")) return "";
            auto links = obj->getObject("_links");
            if (!links->has("next")) return "";
            std::string next = links->getValue<std::string>("next");
            if (next.empty() || next[0] != '/') return "";
            return next;
        } catch (...) {
            return "";
        }
    }

    std::vector<std::string> fetchAllDirectChildPageIds(const std::string& pageId) const {
        std::vector<std::string> all;
        if (apiType_ == ApiType::Cloud) {
            std::string nextPath = "/wiki/api/v2/pages/" + pageId + "/children?limit=250";
            for (int i = 0; i < 1000; ++i) {
                std::string json = doGet(nextPath);
                std::vector<std::string> batch = extractChildPageIds(json);
                all.insert(all.end(), batch.begin(), batch.end());
                nextPath = extractCloudChildrenNextPath(json);
                if (nextPath.empty()) break;
            }
        } else {
            const int limit = 100;
            for (int start = 0, iter = 0; iter < 1000; ++iter) {
                std::string path = "/rest/api/content/" + pageId + "/child/page?limit=" + std::to_string(limit) +
                    "&start=" + std::to_string(start) + "&expand=page";
                std::string json = doGet(path);
                std::vector<std::string> batch = extractChildPageIds(json);
                if (batch.empty()) break;
                all.insert(all.end(), batch.begin(), batch.end());
                if (batch.size() < static_cast<size_t>(limit)) break;
                start += limit;
            }
        }
        return all;
    }

    LoadedConfluencePage loadPageSubtreeImpl(const std::string& pageId, bool resolveIncludes,
                                            std::set<std::string>& visited) const {
        if (visited.count(pageId)) {
            LoadedConfluencePage dup;
            dup.pageId = pageId;
            dup.circularSkip = true;
            return dup;
        }
        visited.insert(pageId);

        std::string json = resolveIncludes ? getPageWithIncludes(pageId) : getPageById(pageId);
        LoadedConfluencePage node;
        node.pageId = pageId;
        node.pageJson = json;

        try {
            std::vector<std::string> childIds = fetchAllDirectChildPageIds(pageId);
            for (const auto& cid : childIds) {
                node.children.push_back(
                    std::make_unique<LoadedConfluencePage>(loadPageSubtreeImpl(cid, resolveIncludes, visited)));
            }
        } catch (...) {}

        return node;
    }

    static std::string normalizeUrl(const std::string& url) {
        if (url.empty()) return "";
        std::string result = url;
        while (!result.empty() && result.back() == '/') {
            result.pop_back();
        }
        return result;
    }

    enum class ApiType { Cloud, Server };

    static ApiType parseApiType(const std::string& s) {
        std::string lower;
        for (char c : s) {
            lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        while (!lower.empty() && (lower.front() == ' ' || lower.front() == '\t')) lower.erase(0, 1);
        while (!lower.empty() && (lower.back() == ' ' || lower.back() == '\t')) lower.pop_back();
        std::string compact;
        for (char c : lower) {
            if (c != '-' && c != '_') compact += c;
        }
        if (compact == "cloud" || compact == "v2") return ApiType::Cloud;
        // server, onprem, datacenter, dc, empty, unknown -> Confluence Server / Data Center (on-prem)
        return ApiType::Server;
    }

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
            return val > 0 ? val : 30;
        } catch (...) {
            return 30;
        }
    }

    static std::string extractPageIdFromUrl(const std::string& url) {
        // Confluence URLs: .../pages/123456789/... or .../pages/123456789
        std::regex re("/pages/(\\d+)(?:/|$|\\?)");
        std::smatch match;
        if (std::regex_search(url, match, re)) {
            return match[1].str();
        }
        return "";
    }

    static std::vector<std::pair<std::string, std::string>> extractAllAttachments(const std::string& jsonStr,
                                                                                   const std::string& pageId) {
        std::vector<std::pair<std::string, std::string>> out;
        try {
            Poco::JSON::Parser parser;
            auto result = parser.parse(jsonStr);
            auto obj = result.extract<Poco::JSON::Object::Ptr>();
            if (!obj->has("results")) return out;
            auto results = obj->getArray("results");
            for (size_t i = 0; i < results->size(); ++i) {
                auto item = results->getObject(i);
                if (!item->has("title")) continue;
                std::string title = item->getValue<std::string>("title");
                std::string path;
                if (item->has("_links")) {
                    auto links = item->getObject("_links");
                    if (links->has("download")) {
                        path = links->getValue<std::string>("download");
                        if (!path.empty() && path[0] != '/') path = "/" + path;
                    }
                }
                if (path.empty()) {
                    std::string encoded;
                    Poco::URI::encode(title, "", encoded);
                    path = "/download/attachments/" + pageId + "/" + encoded;
                }
                if (!path.empty()) out.emplace_back(title, path);
            }
        } catch (...) {}
        return out;
    }

    static std::string findDrawioAttachmentDownloadPath(const std::string& jsonStr, const std::string& diagramName,
                                                        const std::string& pageId) {
        try {
            Poco::JSON::Parser parser;
            auto result = parser.parse(jsonStr);
            auto obj = result.extract<Poco::JSON::Object::Ptr>();
            if (!obj->has("results")) return "";
            auto results = obj->getArray("results");
            std::string nameLower = diagramName;
            for (char& c : nameLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (nameLower.size() >= 7 && nameLower.substr(nameLower.size() - 7) == ".drawio")
                nameLower = nameLower.substr(0, nameLower.size() - 7);
            else if (nameLower.size() >= 4 && nameLower.substr(nameLower.size() - 4) == ".xml")
                nameLower = nameLower.substr(0, nameLower.size() - 4);
            std::string nameWithDrawio = nameLower + ".drawio";
            std::string nameWithXml = nameLower + ".xml";
            for (size_t i = 0; i < results->size(); ++i) {
                auto item = results->getObject(i);
                if (!item->has("title")) continue;
                std::string title = item->getValue<std::string>("title");
                std::string titleLower = title;
                for (char& c : titleLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                size_t dot = titleLower.rfind('.');
                std::string baseName = (dot != std::string::npos) ? titleLower.substr(0, dot) : titleLower;
                bool isDrawio = (titleLower.size() >= 7 && titleLower.substr(titleLower.size() - 7) == ".drawio") ||
                               (titleLower.size() >= 4 && titleLower.substr(titleLower.size() - 4) == ".xml");
                bool nameMatch = (baseName == nameLower || nameLower == baseName) ||
                                 (baseName.find(nameLower) != std::string::npos || nameLower.find(baseName) != std::string::npos) ||
                                 (titleLower == nameWithDrawio || titleLower == nameWithXml);
                if (isDrawio && nameMatch) {
                    if (item->has("_links")) {
                        auto links = item->getObject("_links");
                        if (links->has("download")) {
                            std::string path = links->getValue<std::string>("download");
                            if (!path.empty()) {
                                if (path[0] != '/') path = "/" + path;
                                return path;
                            }
                        }
                    }
                    std::string encoded;
                    Poco::URI::encode(title, "", encoded);
                    return "/download/attachments/" + pageId + "/" + encoded;
                }
                if (nameMatch && !isDrawio) {
                    std::string encoded;
                    Poco::URI::encode(title, "", encoded);
                    return "/download/attachments/" + pageId + "/" + encoded;
                }
            }
            return "";
        } catch (...) {
            return "";
        }
    }

    static std::string extractBodyFromPageJson(const std::string& jsonStr) {
        try {
            Poco::JSON::Parser parser;
            auto result = parser.parse(jsonStr);
            auto obj = result.extract<Poco::JSON::Object::Ptr>();
            if (!obj->has("body")) return "";
            auto body = obj->getObject("body");
            if (!body->has("storage")) return "";
            auto storage = body->getObject("storage");
            if (!storage->has("value")) return "";
            return storage->getValue<std::string>("value");
        } catch (...) {
            return "";
        }
    }

    static std::string replaceBodyInPageJson(const std::string& jsonStr, const std::string& newBody) {
        try {
            Poco::JSON::Parser parser;
            auto result = parser.parse(jsonStr);
            auto obj = result.extract<Poco::JSON::Object::Ptr>();
            if (!obj->has("body")) return jsonStr;
            auto body = obj->getObject("body");
            if (!body->has("storage")) return jsonStr;
            auto storage = body->getObject("storage");
            storage->set("value", newBody);
            std::stringstream ss;
            Poco::JSON::Stringifier::stringify(obj, ss);
            return ss.str();
        } catch (...) {
            return jsonStr;
        }
    }

    struct IncludeMacro {
        std::string pageId;
        std::string fullMacro;
    };

    static std::vector<IncludeMacro> findIncludeMacros(const std::string& html) {
        std::vector<IncludeMacro> result;
        // Match <ac:structured-macro ac:name="include" ...>...</ac:structured-macro>
        // Use [\s\S] instead of . for multiline (no dotall in older C++/GCC)
        std::regex macroRe("<ac:structured-macro\\s+ac:name=\"include\"[^>]*>[\\s\\S]*?</ac:structured-macro>",
                          std::regex::icase);
        std::sregex_iterator it(html.begin(), html.end(), macroRe);
        std::sregex_iterator end;
        for (; it != end; ++it) {
            std::string fullMacro = (*it).str();
            std::string pageId;
            std::smatch m;
            // 1) ri:content-id (Confluence standard for include macro)
            if (std::regex_search(fullMacro, m, std::regex("ri:content-id=\"([^\"]*)\""))) {
                pageId = m[1].str();
            }
            // 2) ri:page-id (alternative in some Confluence versions)
            if (pageId.empty() && std::regex_search(fullMacro, m, std::regex("ri:page-id=\"([^\"]*)\""))) {
                pageId = m[1].str();
            }
            // 3) ac:parameter ac:name="page" with text content (e.g. numeric ID)
            if (pageId.empty() && std::regex_search(fullMacro, m, std::regex("<ac:parameter\\s+ac:name=\"page\"[^>]*>([^<]*)</ac:parameter>", std::regex::icase))) {
                pageId = m[1].str();
                size_t start = pageId.find_first_not_of(" \t\n\r");
                if (start != std::string::npos) {
                    pageId = pageId.substr(start, pageId.find_last_not_of(" \t\n\r") - start + 1);
                }
            }
            // 4) ri:content-title (page title - requires search by title)
            if (pageId.empty() && std::regex_search(fullMacro, m, std::regex("ri:content-title=\"([^\"]*)\""))) {
                pageId = m[1].str();
            }
            if (!pageId.empty()) {
                result.push_back({pageId, fullMacro});
            }
        }
        return result;
    }

    static bool isNumericId(const std::string& s) {
        if (s.empty()) return false;
        for (char c : s) {
            if (c < '0' || c > '9') return false;
        }
        return true;
    }

    std::string resolvePageId(const std::string& pageIdOrTitle) const {
        if (isNumericId(pageIdOrTitle)) {
            return pageIdOrTitle;
        }
        return getPageIdByTitle(pageIdOrTitle);
    }

    std::string getPageIdByTitle(const std::string& title) const {
        if (apiType_ != ApiType::Server) {
            return "";  // Title search is Server API; Cloud has different search
        }
        try {
            std::string encodedTitle;
            Poco::URI::encode(title, "", encodedTitle);
            std::string path = "/rest/api/content?title=" + encodedTitle + "&limit=1";
            std::string json = doGet(path);
            Poco::JSON::Parser parser;
            auto result = parser.parse(json);
            auto obj = result.extract<Poco::JSON::Object::Ptr>();
            if (!obj->has("results")) return "";
            auto results = obj->getArray("results");
            if (results->size() == 0) return "";
            auto first = results->getObject(0);
            if (first->has("id")) {
                return first->getValue<std::string>("id");
            }
            if (first->has("content")) {
                auto content = first->getObject("content");
                if (content->has("id")) {
                    return content->getValue<std::string>("id");
                }
            }
            return "";
        } catch (...) {
            return "";
        }
    }

    std::string processIncludesRecursively(std::string content,
                                          std::set<std::string>& processed) const {
        auto macros = findIncludeMacros(content);
        if (macros.empty()) return content;

        auto& logger = Poco::Logger::get("ConfluenceClient");
        for (const auto& macro : macros) {
            const std::string& pageIdOrTitle = macro.pageId;
            std::string resolvedId = resolvePageId(pageIdOrTitle);
            if (resolvedId.empty()) {
                std::string errMsg = "Cannot resolve page (title or ID): " + pageIdOrTitle;
                logger.error("%s", errMsg.c_str());
                std::string errHtml = "<div class=\"include-error\">[Error: page not found: " + pageIdOrTitle + "]</div>";
                size_t pos = 0;
                while ((pos = content.find(macro.fullMacro, pos)) != std::string::npos) {
                    content.replace(pos, macro.fullMacro.size(), errHtml);
                    pos += errHtml.size();
                }
                continue;
            }
            if (processed.count(resolvedId)) {
                logger.warning("Circular include detected for page: %s", resolvedId.c_str());
                continue;
            }
            std::string includedHtml;
            try {
                std::string subJson = getPageById(resolvedId);
                includedHtml = extractBodyFromPageJson(subJson);
            } catch (const std::exception& e) {
                std::string loadErr = "Failed to load included page " + resolvedId +
                    " (resolved from " + pageIdOrTitle + "): " + (e.what() ? e.what() : "unknown");
                logger.error("%s", loadErr.c_str());
                includedHtml = "<div class=\"include-error\">[Error loading included page: " + pageIdOrTitle + "]</div>";
            }
            if (!includedHtml.empty()) {
                processed.insert(resolvedId);
                std::set<std::string> subProcessed(processed);
                includedHtml = processIncludesRecursively(includedHtml, subProcessed);
                processed = subProcessed;
            }
            // Replace macro with content (simple string replace - macro is exact match)
            size_t pos = 0;
            while ((pos = content.find(macro.fullMacro, pos)) != std::string::npos) {
                content.replace(pos, macro.fullMacro.size(), includedHtml);
                pos += includedHtml.size();
            }
        }
        return content;
    }

    std::string doGet(const std::string& path) const {
        if (token_.empty()) {
            throw std::runtime_error("CONFLUENCE_TOKEN is not set");
        }
        if (baseUrl_.empty()) {
            throw std::runtime_error("CONFLUENCE_URL is not set");
        }

        Poco::URI baseUri(baseUrl_);
        std::string fullPath = path;
        if (fullPath.empty() || fullPath[0] != '/') {
            fullPath = "/" + fullPath;
        }
        // On-premises installs often use a context path (e.g. https://host/confluence). Prepend it when the
        // request path does not already start with that prefix. Plain https://host uses empty path — no change.
        std::string basePath = baseUri.getPath();
        if (!basePath.empty() && basePath != "/") {
            if (basePath.back() == '/') basePath.pop_back();
            const bool alreadyPrefixed = (fullPath.size() >= basePath.size() &&
                fullPath.compare(0, basePath.size(), basePath) == 0);
            if (!alreadyPrefixed) fullPath = basePath + fullPath;
        }

        Poco::Net::Context::Ptr sslContext;
        if (verifySsl_) {
            sslContext = new Poco::Net::Context(Poco::Net::Context::CLIENT_USE, "", "", "",
                                                Poco::Net::Context::VERIFY_STRICT);
        } else {
            sslContext = new Poco::Net::Context(Poco::Net::Context::CLIENT_USE, "", "", "",
                                                Poco::Net::Context::VERIFY_NONE);
        }

        std::string host = baseUri.getHost();
        int port = baseUri.getPort();
        if (port <= 0) port = 443;
        std::string hostHeader = (port == 443) ? host : (host + ":" + std::to_string(port));

        Poco::Net::HTTPSClientSession session(host, port, sslContext);
        session.setTimeout(Poco::Timespan(timeoutSec_, 0));

        Poco::Net::HTTPRequest req(Poco::Net::HTTPRequest::HTTP_GET, fullPath,
                                   Poco::Net::HTTPMessage::HTTP_1_1);
        req.setHost(hostHeader);
        req.set("Accept", "application/json");
        req.set("Authorization", buildAuthHeader());

        auto& logger = Poco::Logger::get("ConfluenceClient");
        std::string fullUrl = baseUri.getScheme() + "://" + hostHeader + fullPath;
        logger.information("Confluence request: GET %s", fullUrl);

        session.sendRequest(req);

        Poco::Net::HTTPResponse res;
        std::istream& rs = session.receiveResponse(res);

        std::string responseBody;
        Poco::StreamCopier::copyToString(rs, responseBody);

        if (res.getStatus() != Poco::Net::HTTPResponse::HTTP_OK) {
            logger.error("Confluence API error: %d - %s (request was: GET %s)", static_cast<int>(res.getStatus()), responseBody, fullUrl);
            throw std::runtime_error("Confluence API error: " + std::to_string(res.getStatus()) + " - " + fullUrl);
        }

        return responseBody;
    }

    std::string buildAuthHeader() const {
        if (!user_.empty()) {
            std::string credentials = user_ + ":" + token_;
            std::stringstream ss;
            Poco::Base64Encoder encoder(ss);
            encoder.rdbuf()->setLineLength(0);
            encoder << credentials;
            encoder.close();
            std::string encoded = ss.str();
            encoded.erase(std::remove_if(encoded.begin(), encoded.end(),
                [](char c) { return c == '\n' || c == '\r'; }), encoded.end());
            return "Basic " + encoded;
        }
        return "Bearer " + token_;
    }

    std::string baseUrl_;
    std::string user_;
    std::string token_;
    ApiType apiType_;
    int timeoutSec_;
    bool verifySsl_;
};

} // namespace confluence
