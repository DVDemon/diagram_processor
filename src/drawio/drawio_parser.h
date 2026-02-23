#pragma once

#include <Poco/JSON/Object.h>
#include <Poco/JSON/Array.h>
#include <Poco/JSON/Stringifier.h>
#include <Poco/Base64Decoder.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include <zlib.h>

namespace drawio {

/**
 * Parser for DrawIO (diagrams.net) C4 diagrams.
 * Based on https://github.com/DVDemon/drawio
 * Handles graphic errors: lines not touching rectangles, nested rectangles.
 * Output: components (id, code, name), requests (request_id, source_id, target_id, description).
 */
class DrawioParser {
public:
    static std::string parse(const std::string& xmlInput) {
        std::string xml = xmlInput;
        if (xml.find("<mxfile") == std::string::npos && xml.find("<mxGraphModel") == std::string::npos) {
            Poco::JSON::Object err;
            err.set("error", "Not a DrawIO diagram. Expected mxfile or mxGraphModel XML.");
            err.set("success", false);
            return toJson(err);
        }

        std::string rootXml = extractRootXml(xml);
        if (rootXml.empty()) {
            Poco::JSON::Object err;
            err.set("error", "Could not extract diagram content from DrawIO XML.");
            err.set("success", false);
            return toJson(err);
        }

        std::map<std::string, Component> components;
        std::vector<Relation> relations;
        std::vector<BrokenRelation> broken;

        parseObjects(rootXml, components, relations, broken);
        parseMxCells(rootXml, broken);
        applyLabelsToBroken(rootXml, broken);
        fillParentId(components);
        fixBrokenRelations(components, relations, broken);
        relations = fixMissingRelations(components, relations);

        return buildJson(components, relations);
    }

private:
    struct Component {
        std::string id;
        std::string c4Name;
        std::string c4Type;
        std::string c4Description;
        double left = 0, top = 0, right = 0, bottom = 0;
        std::string parentId;
        bool hasGeometry = false;
    };

    struct Relation {
        std::string source;
        std::string target;
        std::string c4Description;
    };

    struct BrokenRelation {
        std::string id;
        std::string source;
        std::string target;
        double srcX = 0, srcY = 0;
        double tgtX = 0, tgtY = 0;
        bool hasSrcPoint = false;
        bool hasTgtPoint = false;
        std::string c4Description;
    };

    static std::string extractRootXml(const std::string& xml) {
        // Use string search instead of regex to avoid stack overflow on large diagrams
        size_t diagStart = findCaseInsensitive(xml, "<diagram");
        if (diagStart != std::string::npos) {
            size_t tagEnd = xml.find('>', diagStart);
            if (tagEnd != std::string::npos) {
                size_t diagEnd = findCaseInsensitive(xml, "</diagram>", tagEnd);
                if (diagEnd != std::string::npos) {
                    std::string diagramContent = xml.substr(tagEnd + 1, diagEnd - tagEnd - 1);
                    trim(diagramContent);

                    if (!diagramContent.empty()) {
                        if (diagramContent[0] == '<') {
                            return diagramContent;
                        }

                        std::string decoded;
                        try {
            std::string b64 = diagramContent;
            std::istringstream is(b64);
            Poco::Base64Decoder dec(is);
            std::ostringstream os;
            char buf[4096];
            while (dec.read(buf, sizeof(buf)) && dec.gcount() > 0) {
                os.write(buf, dec.gcount());
            }
            std::string compressed = os.str();
            std::vector<unsigned char> compressedVec(compressed.begin(), compressed.end());

            std::vector<unsigned char> out(compressed.size() * 4);
            z_stream strm = {};
            strm.zalloc = Z_NULL;
            strm.zfree = Z_NULL;
            strm.opaque = Z_NULL;
            strm.avail_in = static_cast<uInt>(compressedVec.size());
            strm.next_in = compressedVec.data();

            if (inflateInit2(&strm, -15) != Z_OK) return "";

            strm.avail_out = static_cast<uInt>(out.size());
            strm.next_out = out.data();

            inflate(&strm, Z_FINISH);
            inflateEnd(&strm);

            decoded.assign(reinterpret_cast<char*>(out.data()), strm.total_out);

            std::string unescaped;
            for (size_t i = 0; i < decoded.size(); ++i) {
                if (decoded[i] == '%' && i + 2 < decoded.size()) {
                    int v = 0;
                    if (sscanf(decoded.c_str() + i + 1, "%2x", &v) == 1) {
                        unescaped += static_cast<char>(v);
                        i += 2;
                        continue;
                    }
                }
                unescaped += decoded[i];
            }
                        return unescaped;
                        } catch (...) {
                            return "";
                        }
                    }
                }
            }
        }
        if (xml.find("<root>") != std::string::npos) return xml;
        return "";
    }

    static size_t findCaseInsensitive(const std::string& haystack, const std::string& needle, size_t pos = 0) {
        if (needle.empty() || pos >= haystack.size()) return std::string::npos;
        for (; pos <= haystack.size() - needle.size(); ++pos) {
            bool match = true;
            for (size_t i = 0; i < needle.size(); ++i) {
                char c1 = haystack[pos + i];
                char c2 = needle[i];
                if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
                if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
                if (c1 != c2) { match = false; break; }
            }
            if (match) return pos;
        }
        return std::string::npos;
    }

    static void parseObjects(const std::string& rootXml,
                            std::map<std::string, Component>& components,
                            std::vector<Relation>& relations,
                            std::vector<BrokenRelation>& broken) {
        size_t pos = 0;
        while ((pos = findCaseInsensitive(rootXml, "<object", pos)) != std::string::npos) {
            size_t tagEnd = rootXml.find('>', pos);
            if (tagEnd == std::string::npos) break;
            std::string attrsStr = rootXml.substr(pos + 7, tagEnd - pos - 7);
            size_t endTag = findCaseInsensitive(rootXml, "</object>", tagEnd);
            if (endTag == std::string::npos) break;
            std::string objBlock = rootXml.substr(pos, endTag + 9 - pos);
            pos = endTag + 9;

            auto attrs = parseAttrsStatic(attrsStr);

            std::string c4Type = getAttr(attrs, "c4Type");
            if (c4Type == "Relationship") {
                std::string mxCellBlock = findTagContent(objBlock, "mxCell");
                if (mxCellBlock.empty()) mxCellBlock = objBlock;
                auto cellAttrs = parseAttrsStatic(extractAttrsFromTag(mxCellBlock, "mxCell"));
                std::string src = getAttr(cellAttrs, "source");
                std::string tgt = getAttr(cellAttrs, "target");

                if (!src.empty() && !tgt.empty()) {
                    Relation r;
                    r.source = src;
                    r.target = tgt;
                    r.c4Description = getAttr(attrs, "c4Description");
                    relations.push_back(r);
                } else {
                    BrokenRelation br;
                    br.id = getAttr(attrs, "id");
                    br.source = src;
                    br.target = tgt;
                    br.c4Description = getAttr(attrs, "c4Description");
                    std::string geomBlock = findTagContent(mxCellBlock, "mxGeometry");
                    parseGeometryPoints(geomBlock, br);
                    broken.push_back(br);
                }
            } else {
                Component c;
                c.id = getAttr(attrs, "id");
                c.c4Name = getAttr(attrs, "c4Name");
                c.c4Type = c4Type.empty() ? "Component" : c4Type;
                c.c4Description = getAttr(attrs, "c4Description");
                if (c.c4Name.empty()) c.c4Name = c.c4Description.empty() ? "Component_" + c.id : c.c4Description;

                std::string mxCellBlock = findTagContent(objBlock, "mxCell");
                if (mxCellBlock.empty()) mxCellBlock = objBlock;
                std::string geomBlock = findTagContent(mxCellBlock, "mxGeometry");
                if (geomBlock.empty()) geomBlock = mxCellBlock;
                auto ga = parseAttrsStatic(extractAttrsFromTag(geomBlock, "mxGeometry"));
                if (!ga.empty() || geomBlock.find("mxGeometry") != std::string::npos) {
                    c.left = parseDouble(getAttr(ga, "x"), 0);
                    c.top = parseDouble(getAttr(ga, "y"), 0);
                    double w = parseDouble(getAttr(ga, "width"), 0);
                    double h = parseDouble(getAttr(ga, "height"), 0);
                    c.right = c.left + w;
                    c.bottom = c.top + h;
                    c.hasGeometry = true;
                }
                if (!c.id.empty()) components[c.id] = c;
            }
        }
    }

    static void parseGeometryPoints(const std::string& geomBlock, BrokenRelation& br) {
        auto ga = parseAttrsFromTag(geomBlock, "mxGeometry");
        br.srcX = parseDouble(getAttr(ga, "x"), 0);
        br.srcY = parseDouble(getAttr(ga, "y"), 0);
        br.tgtX = br.srcX;
        br.tgtY = br.srcY;

        std::regex mxPtRe(R"(<mxPoint\s+([^>]*)/?>)", std::regex::icase);
        std::sregex_iterator it(geomBlock.begin(), geomBlock.end(), mxPtRe);
        for (; it != std::sregex_iterator(); ++it) {
            auto a = parseAttrsStatic((*it)[1].str());
            std::string as = getAttr(a, "as");
            double x = parseDouble(getAttr(a, "x"), 0);
            double y = parseDouble(getAttr(a, "y"), 0);
            if (as == "source" || as == "sourcePoint") {
                br.srcX = x;
                br.srcY = y;
                br.hasSrcPoint = true;
            } else if (as == "target" || as == "targetPoint") {
                br.tgtX = x;
                br.tgtY = y;
                br.hasTgtPoint = true;
            }
        }
    }

    static std::map<std::string, std::string> parseAttrsFromTag(const std::string& block, const std::string& tag) {
        std::regex re(tag + R"(\s+([^>]*)/?>)", std::regex::icase);
        std::smatch m;
        if (std::regex_search(block, m, re)) return parseAttrsStatic(m[1].str());
        return {};
    }

    static std::map<std::string, std::string> parseAttrsStatic(const std::string& attrs) {
        std::map<std::string, std::string> m;
        std::regex kvRe(R"((\w+)=["']([^"']*)["'])");
        std::sregex_iterator it(attrs.begin(), attrs.end(), kvRe);
        for (; it != std::sregex_iterator(); ++it) m[(*it)[1].str()] = (*it)[2].str();
        return m;
    }

    static void parseMxCells(const std::string& rootXml,
                            std::vector<BrokenRelation>& broken) {
        size_t pos = 0;
        while ((pos = findCaseInsensitive(rootXml, "<mxCell", pos)) != std::string::npos) {
            size_t tagEnd = rootXml.find('>', pos);
            if (tagEnd == std::string::npos) break;
            std::string attrsStr = rootXml.substr(pos + 7, tagEnd - pos - 7);
            pos = tagEnd + 1;

            auto a = parseAttrsStatic(attrsStr);
            std::string style = getAttr(a, "style");
            if (style.find("edgeStyle=") != std::string::npos) {
                BrokenRelation br;
                br.id = getAttr(a, "id");
                br.source = getAttr(a, "source");
                br.target = getAttr(a, "target");
                br.c4Description = "";
                broken.push_back(br);
            }
        }
    }

    static void applyLabelsToBroken(const std::string& rootXml, std::vector<BrokenRelation>& broken) {
        std::map<std::string, std::string> labels;
        size_t pos = 0;
        while ((pos = findCaseInsensitive(rootXml, "<mxCell", pos)) != std::string::npos) {
            size_t tagEnd = rootXml.find('>', pos);
            if (tagEnd == std::string::npos) break;
            std::string attrsStr = rootXml.substr(pos + 7, tagEnd - pos - 7);
            pos = tagEnd + 1;

            auto a = parseAttrsStatic(attrsStr);
            if (getAttr(a, "style").find("edgeLabel") != std::string::npos) {
                std::string parent = getAttr(a, "parent");
                std::string value = getAttr(a, "value");
                if (!parent.empty() && !value.empty()) labels[parent] = value;
            }
        }
        for (auto& br : broken) {
            if (labels.count(br.id)) br.c4Description = labels[br.id];
        }
    }

    static bool isInside(const Component& inner, const Component& outer) {
        if (!inner.hasGeometry || !outer.hasGeometry) return false;
        return inner.left >= outer.left && inner.top >= outer.top &&
               inner.right <= outer.right && inner.bottom <= outer.bottom;
    }

    static void fillParentId(std::map<std::string, Component>& components) {
        for (auto& kv : components) {
            Component& c = kv.second;
            for (const auto& pkv : components) {
                if (pkv.first == c.id) continue;
                const Component& parent = pkv.second;
                if (isInside(c, parent)) {
                    c.parentId = parent.id;
                    break;
                }
            }
        }
    }

    static void fixBrokenRelations(const std::map<std::string, Component>& components,
                                  std::vector<Relation>& relations,
                                  std::vector<BrokenRelation>& broken) {
        for (const auto& br : broken) {
            std::string src = br.source;
            std::string tgt = br.target;

            if (src.empty() && br.hasSrcPoint) {
                double bestArea = 1e30;
                for (const auto& kv : components) {
                    if (!kv.second.hasGeometry) continue;
                    const auto& c = kv.second;
                    if (br.srcX >= c.left && br.srcX <= c.right && br.srcY >= c.top && br.srcY <= c.bottom) {
                        double area = (c.right - c.left) * (c.bottom - c.top);
                        if (area < bestArea) { bestArea = area; src = c.id; }
                    }
                }
            }
            if (tgt.empty() && br.hasTgtPoint) {
                double bestArea = 1e30;
                for (const auto& kv : components) {
                    if (!kv.second.hasGeometry) continue;
                    const auto& c = kv.second;
                    if (br.tgtX >= c.left && br.tgtX <= c.right && br.tgtY >= c.top && br.tgtY <= c.bottom) {
                        double area = (c.right - c.left) * (c.bottom - c.top);
                        if (area < bestArea) { bestArea = area; tgt = c.id; }
                    }
                }
            }

            if (!src.empty() && !tgt.empty()) {
                Relation r;
                r.source = src;
                r.target = tgt;
                r.c4Description = br.c4Description;
                relations.push_back(r);
            }
        }
    }

    static std::vector<Relation> fixMissingRelations(const std::map<std::string, Component>& components,
                                                     const std::vector<Relation>& relations) {
        std::vector<Relation> out;
        for (const auto& r : relations) {
            if (components.count(r.source) && components.count(r.target))
                out.push_back(r);
        }
        return out;
    }

    static std::string buildJson(const std::map<std::string, Component>& components,
                                 const std::vector<Relation>& relations) {
        Poco::JSON::Object root;
        root.set("success", true);

        Poco::JSON::Array compArr;
        for (const auto& p : components) {
            if (p.second.c4Type == "SystemScopeBoundary" || p.second.c4Type == "ContainerScopeBoundary") continue;
            Poco::JSON::Object c;
            c.set("id", p.second.id);
            c.set("code", p.second.id);
            std::string name = p.second.c4Name.empty() ? (p.second.c4Description.empty() ? p.second.c4Type : p.second.c4Description) : p.second.c4Name;
            c.set("name", name);
            c.set("c4_type", p.second.c4Type);
            compArr.add(c);
        }
        root.set("components", compArr);

        Poco::JSON::Array reqArr;
        int step = 0;
        for (const auto& r : relations) {
            Poco::JSON::Object req;
            req.set("request_id", ++step);
            req.set("component_source_id", r.source);
            req.set("component_target_id", r.target);
            req.set("description", r.c4Description);
            reqArr.add(req);
        }
        root.set("requests", reqArr);

        Poco::JSON::Array hierarchyArr;
        int hStep = 0;
        for (const auto& p : components) {
            if (p.second.c4Type == "SystemScopeBoundary" || p.second.c4Type == "ContainerScopeBoundary") continue;
            if (!p.second.parentId.empty() && components.count(p.second.parentId)) {
                const auto& parent = components.at(p.second.parentId);
                if (parent.c4Type != "SystemScopeBoundary" && parent.c4Type != "ContainerScopeBoundary") {
                    Poco::JSON::Object h;
                    h.set("hierarchy_id", ++hStep);
                    h.set("parent_id", p.second.parentId);
                    h.set("child_id", p.second.id);
                    hierarchyArr.add(h);
                }
            }
        }
        root.set("parent_child", hierarchyArr);

        return toJson(root);
    }

    static std::string findTagContent(const std::string& block, const std::string& tag) {
        std::regex re("<" + tag + R"(\s+[^>]*>([\s\S]*?)</)" + tag + ">", std::regex::icase);
        std::smatch m;
        if (std::regex_search(block, m, re)) return m[1].str();
        std::regex re2("<" + tag + R"(\s+[^/]*/>)", std::regex::icase);
        if (std::regex_search(block, m, re2)) return "";
        return "";
    }

    static std::string extractAttrsFromTag(const std::string& block, const std::string& tag) {
        std::regex re("<" + tag + R"(\s+[^>/]*(?:>|/))", std::regex::icase);
        std::smatch m;
        if (std::regex_search(block, m, re)) {
            std::string a = m[1].str();
            if (a.back() == '/') a.pop_back();
            if (a.back() == '>') a.pop_back();
            return a;
        }
        return "";
    }

    static std::string getAttr(const std::map<std::string, std::string>& m,
                              const std::string& k,
                              const std::string& def = "") {
        auto it = m.find(k);
        return it != m.end() ? it->second : def;
    }

    static double parseDouble(const std::string& s, double def) {
        if (s.empty()) return def;
        try { return std::stod(s); } catch (...) { return def; }
    }

    static void trim(std::string& s) {
        size_t p = s.find_first_not_of(" \t\n\r");
        if (p != std::string::npos) s.erase(0, p);
        p = s.find_last_not_of(" \t\n\r");
        if (p != std::string::npos) s.erase(p + 1);
    }

    static std::string toJson(const Poco::JSON::Object& obj) {
        std::stringstream ss;
        Poco::JSON::Stringifier::stringify(obj, ss);
        return ss.str();
    }
};

} // namespace drawio
