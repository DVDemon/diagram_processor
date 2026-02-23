#pragma once

#include <Poco/JSON/Object.h>
#include <Poco/JSON/Array.h>
#include <Poco/JSON/Stringifier.h>

#include <algorithm>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace plantuml {

/**
 * Parser for PlantUML C4 diagrams.
 * Extracts systems/containers (rectangle "==Name..." <<X>> as X) and dependencies
 * (source .[#...].> target : "Message").
 * Outputs JSON with components (id, code, name, c4_type), requests, and parent_child hierarchy.
 */
class PlantUmlC4Parser {
public:
    /**
     * Parse PlantUML text and return JSON.
     * Only processes C4 diagrams. Returns error JSON if not a C4 diagram.
     * @param plantumlText Raw PlantUML diagram text
     * @return JSON string with components, requests, and parent_child
     */
    static std::string parse(const std::string& plantumlText) {
        if (!isC4Diagram(plantumlText)) {
            Poco::JSON::Object err;
            err.set("error", "Not a C4 diagram. Expected rectangle systems and .[].> dependencies.");
            err.set("success", false);
            std::stringstream ss;
            Poco::JSON::Stringifier::stringify(err, ss);
            return ss.str();
        }

        std::map<std::string, ComponentInfo> components;
        std::vector<Dependency> dependencies;

        extractSystems(plantumlText, components);
        extractDependencies(plantumlText, components, dependencies);

        return buildJson(components, dependencies);
    }

private:
    struct ComponentInfo {
        std::string id;
        std::string name;
        std::string c4_type;
        std::string parentId;
    };

    struct Dependency {
        std::string source;
        std::string target;
        std::string description;
        int step;
    };

    static bool isC4Diagram(const std::string& text) {
        std::string lower = text;
        for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        bool hasRectangle = (lower.find("rectangle") != std::string::npos);
        bool hasC4Dep = (lower.find("].[") != std::string::npos && lower.find("].>") != std::string::npos);
        bool hasPerson = (lower.find("person(") != std::string::npos);
        bool hasSystem = (lower.find("system(") != std::string::npos || lower.find("system_ext(") != std::string::npos);
        bool hasRel = (lower.find("rel(") != std::string::npos);
        bool hasC4Indicators = (lower.find("software system") != std::string::npos ||
                               lower.find("container") != std::string::npos ||
                               lower.find("person") != std::string::npos ||
                               lower.find("system boundary") != std::string::npos);
        return (hasRectangle && hasC4Dep) || (hasRectangle && hasC4Indicators) ||
               ((hasPerson || hasSystem) && hasRel);
    }

    static void extractSystems(const std::string& text, std::map<std::string, ComponentInfo>& out) {
        extractSystemsRectangle(text, out);
        extractSystemsC4Lib(text, out);
    }

    static std::string parseC4TypeFromLabel(const std::string& label) {
        std::string lower = label;
        for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lower.find("software system") != std::string::npos) return "SoftwareSystem";
        if (lower.find("container") != std::string::npos) return "Container";
        if (lower.find("component") != std::string::npos) return "Component";
        if (lower.find("person") != std::string::npos) return "Person";
        if (lower.find("system boundary") != std::string::npos) return "SystemBoundary";
        if (lower.find("container boundary") != std::string::npos) return "ContainerBoundary";
        return "SoftwareSystem";
    }

    static void extractSystemsRectangle(const std::string& text, std::map<std::string, ComponentInfo>& out) {
        std::regex re(R"re(rectangle\s+"==([^"\\]+)(?:\\n<size:\d+>\[([^\]]+)\]</size>)?(?:\\n.*?)?"\s+<<[^>]+>>\s+as\s+([^\s]+))re", std::regex::icase);
        std::sregex_iterator it(text.begin(), text.end(), re);
        for (; it != std::sregex_iterator(); ++it) {
            std::string name = trim((*it)[1].str());
            std::string alias = trim((*it)[3].str());
            std::string typeLabel = (*it)[2].matched ? trim((*it)[2].str()) : "";
            if (!alias.empty()) {
                ComponentInfo c;
                c.id = alias;
                c.name = name;
                c.c4_type = typeLabel.empty() ? "SoftwareSystem" : parseC4TypeFromLabel(typeLabel);
                out[alias] = c;
            }
        }
        std::regex re2(R"re(rectangle\s+"==([^"\\]+)(?:\\n.*?)?"\s+<<[^>]+>>\s+as\s+([^\s]+))re", std::regex::icase);
        it = std::sregex_iterator(text.begin(), text.end(), re2);
        for (; it != std::sregex_iterator(); ++it) {
            std::string name = trim((*it)[1].str());
            std::string alias = trim((*it)[2].str());
            if (!alias.empty() && !out.count(alias)) {
                ComponentInfo c;
                c.id = alias;
                c.name = name;
                c.c4_type = "SoftwareSystem";
                out[alias] = c;
            }
        }
    }

    static void extractSystemsC4Lib(const std::string& text, std::map<std::string, ComponentInfo>& out) {
        struct C4LibPattern { std::string regex; std::string c4_type; bool hasParent; };
        std::vector<C4LibPattern> patterns = {
            { R"re(Person\s*\(\s*([^,]+)\s*,\s*"([^"]+)"\s*(?:,\s*"([^"]*)")?\s*\))re", "Person", false },
            { R"re(Person_Ext\s*\(\s*([^,]+)\s*,\s*"([^"]+)"\s*(?:,\s*"([^"]*)")?\s*\))re", "Person", false },
            { R"re(System\s*\(\s*([^,]+)\s*,\s*"([^"]+)"\s*(?:,\s*"([^"]*)")?\s*\))re", "SoftwareSystem", false },
            { R"re(System_Ext\s*\(\s*([^,]+)\s*,\s*"([^"]+)"\s*(?:,\s*"([^"]*)")?\s*\))re", "SoftwareSystem", false },
            { R"re(Container\s*\(\s*([^,]+)\s*,\s*"([^"]+)"\s*(?:,\s*"([^"]*)")?\s*\))re", "Container", true },
            { R"re(Container_Ext\s*\(\s*([^,]+)\s*,\s*"([^"]+)"\s*(?:,\s*"([^"]*)")?\s*\))re", "Container", true },
            { R"re(Component\s*\(\s*([^,]+)\s*,\s*"([^"]+)"\s*(?:,\s*"([^"]*)")?\s*\))re", "Component", true },
            { R"re(Component_Ext\s*\(\s*([^,]+)\s*,\s*"([^"]+)"\s*(?:,\s*"([^"]*)")?\s*\))re", "Component", true },
        };

        for (const auto& p : patterns) {
            std::regex re(p.regex, std::regex::icase);
            std::sregex_iterator it(text.begin(), text.end(), re);
            for (; it != std::sregex_iterator(); ++it) {
                std::string arg1 = trim((*it)[1].str());
                std::string arg2 = trim((*it)[2].str());
                std::string alias = p.hasParent ? arg2 : arg1;
                std::string name = arg2;
                std::string parentId = p.hasParent ? arg1 : "";
                if (!alias.empty()) {
                    ComponentInfo c;
                    c.id = alias;
                    c.name = name;
                    c.c4_type = p.c4_type;
                    c.parentId = parentId;
                    out[alias] = c;
                }
            }
        }
    }

    static void extractDependencies(const std::string& text,
                                   const std::map<std::string, ComponentInfo>& components,
                                   std::vector<Dependency>& out) {
        extractDependenciesArrow(text, components, out);
        extractDependenciesRel(text, components, out);
    }

    static void extractDependenciesArrow(const std::string& text,
                                         const std::map<std::string, ComponentInfo>& components,
                                         std::vector<Dependency>& out) {
        std::regex re(R"re(([^\s]+)\s+\.[^\]]*\]\.>\s+([^\s]+)\s*:\s*"([^"]*)")re");
        std::sregex_iterator it(text.begin(), text.end(), re);
        std::sregex_iterator end;

        struct MatchInfo {
            std::smatch m;
            size_t start;
        };
        std::vector<MatchInfo> matches;
        for (; it != end; ++it) {
            MatchInfo mi;
            mi.m = *it;
            mi.start = it->position(0);
            matches.push_back(mi);
        }

        std::sort(matches.begin(), matches.end(),
                  [](const MatchInfo& a, const MatchInfo& b) { return a.start < b.start; });

        int step = static_cast<int>(out.size());
        for (const auto& mi : matches) {
            std::string src = trim(mi.m[1].str());
            std::string tgt = trim(mi.m[2].str());
            std::string desc = stripHtml(trim(mi.m[3].str()));

            if (components.count(src) && components.count(tgt)) {
                step++;
                out.push_back({src, tgt, desc, step});
            }
        }
    }

    static void extractDependenciesRel(const std::string& text,
                                       const std::map<std::string, ComponentInfo>& components,
                                       std::vector<Dependency>& out) {
        std::regex re(R"re(Rel\s*\(\s*([^,]+)\s*,\s*([^,]+)\s*,\s*"([^"]*)"\s*\))re", std::regex::icase);
        std::sregex_iterator it(text.begin(), text.end(), re);
        std::sregex_iterator end;

        struct MatchInfo {
            std::smatch m;
            size_t start;
        };
        std::vector<MatchInfo> matches;
        for (; it != end; ++it) {
            MatchInfo mi;
            mi.m = *it;
            mi.start = it->position(0);
            matches.push_back(mi);
        }

        std::sort(matches.begin(), matches.end(),
                  [](const MatchInfo& a, const MatchInfo& b) { return a.start < b.start; });

        int step = static_cast<int>(out.size());
        for (const auto& mi : matches) {
            std::string src = trim(mi.m[1].str());
            std::string tgt = trim(mi.m[2].str());
            std::string desc = stripHtml(trim(mi.m[3].str()));

            if (components.count(src) && components.count(tgt)) {
                step++;
                out.push_back({src, tgt, desc, step});
            }
        }
    }

    static std::string stripHtml(const std::string& s) {
        return std::regex_replace(s, std::regex("<[^>]+>"), "");
    }

    static std::string trim(const std::string& s) {
        size_t start = s.find_first_not_of(" \t\n\r");
        if (start == std::string::npos) return "";
        size_t end = s.find_last_not_of(" \t\n\r");
        return s.substr(start, end - start + 1);
    }

    static std::string buildJson(const std::map<std::string, ComponentInfo>& components,
                                 const std::vector<Dependency>& dependencies) {
        Poco::JSON::Object root;
        root.set("success", true);

        Poco::JSON::Array compArr;
        for (const auto& p : components) {
            Poco::JSON::Object c;
            c.set("id", p.second.id);
            c.set("code", p.second.id);
            c.set("name", p.second.name);
            c.set("c4_type", p.second.c4_type);
            compArr.add(c);
        }
        root.set("components", compArr);

        Poco::JSON::Array reqArr;
        for (const auto& r : dependencies) {
            Poco::JSON::Object req;
            req.set("request_id", r.step);
            req.set("component_source_id", r.source);
            req.set("component_target_id", r.target);
            req.set("description", r.description);
            reqArr.add(req);
        }
        root.set("requests", reqArr);

        Poco::JSON::Array hierarchyArr;
        int hStep = 0;
        for (const auto& p : components) {
            if (!p.second.parentId.empty() && components.count(p.second.parentId)) {
                Poco::JSON::Object h;
                h.set("hierarchy_id", ++hStep);
                h.set("parent_id", p.second.parentId);
                h.set("child_id", p.second.id);
                hierarchyArr.add(h);
            }
        }
        root.set("parent_child", hierarchyArr);

        std::stringstream ss;
        Poco::JSON::Stringifier::stringify(root, ss);
        return ss.str();
    }
};

} // namespace plantuml
