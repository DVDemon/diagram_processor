#pragma once

#include <Poco/JSON/Object.h>
#include <Poco/JSON/Array.h>
#include <Poco/JSON/Stringifier.h>

#include <algorithm>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace plantuml {

/**
 * Parser for PlantUML Sequence diagrams.
 * Extracts participants/actors and their interactions, outputs JSON with
 * components (id, code, name) and ordered requests (request_id, source_id, target_id, description).
 */
class PlantUmlSequenceParser {
public:
    /**
     * Parse PlantUML text and return JSON.
     * Only processes Sequence diagrams. Returns error JSON if not a sequence diagram.
     * @param plantumlText Raw PlantUML diagram text
     * @return JSON string with components and requests, or error
     */
    static std::string parse(const std::string& plantumlText) {
        if (!isSequenceDiagram(plantumlText)) {
            Poco::JSON::Object err;
            err.set("error", "Not a sequence diagram. Only participant/actor and -> interactions are supported.");
            err.set("success", false);
            std::stringstream ss;
            Poco::JSON::Stringifier::stringify(err, ss);
            return ss.str();
        }

        std::map<std::string, std::string> participants;
        std::vector<Interaction> interactions;

        extractParticipants(plantumlText, participants);
        extractInteractions(plantumlText, participants, interactions);

        if (participants.empty() && !interactions.empty()) {
            extractParticipantsFromArrows(plantumlText, participants);
        }

        return buildJson(participants, interactions);
    }

private:
    struct Interaction {
        std::string source;
        std::string target;
        std::string message;
        int step;
    };

    static bool isSequenceDiagram(const std::string& text) {
        std::string lower = text;
        for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        bool hasParticipant = (lower.find("participant") != std::string::npos || lower.find("actor") != std::string::npos);
        bool hasArrow = (lower.find("->") != std::string::npos || lower.find("-->") != std::string::npos);
        return hasParticipant || hasArrow;
    }

    static void extractParticipants(const std::string& text, std::map<std::string, std::string>& out) {
        std::regex re(R"re((?:participant|actor)\s+(?:"([^"]+)"|([^\s]+))(?:\s+as\s+([^\s]+))?)re", std::regex::icase);
        std::sregex_iterator it(text.begin(), text.end(), re);
        std::sregex_iterator end;

        for (; it != end; ++it) {
            std::string name = (*it)[1].matched ? (*it)[1].str() : (*it)[2].str();
            std::string alias = (*it)[3].matched ? (*it)[3].str() : name;
            name = trim(name);
            alias = trim(alias);
            if (!alias.empty()) out[alias] = name;
        }
    }

    static void extractParticipantsFromArrows(const std::string& text, std::map<std::string, std::string>& out) {
        std::vector<std::string> patterns = {
            R"(([^\s]+)\s*->\s*([^\s]+))",
            R"(([^\s]+)\s*-->\s*([^\s]+))",
            R"(([^\s]+)\s*->>\s*([^\s]+))",
            R"(([^\s]+)\s*-->>\s*([^\s]+))",
            R"(([^\s]+)\s*<<-\s*([^\s]+))",
            R"(([^\s]+)\s*<--\s*([^\s]+))",
        };

        std::set<std::string> all;
        for (const auto& pat : patterns) {
            std::regex re(pat);
            std::sregex_iterator it(text.begin(), text.end(), re);
            for (; it != std::sregex_iterator(); ++it) {
                std::string src = cleanParticipant((*it)[1].str());
                std::string tgt = cleanParticipant((*it)[2].str());
                if (isValidParticipant(src)) all.insert(src);
                if (isValidParticipant(tgt)) all.insert(tgt);
            }
        }
        for (const auto& p : all) out[p] = p;
    }

    static std::string cleanParticipant(std::string s) {
        s = trim(s);
        std::regex trailing(R"([:\s]+$)");
        s = std::regex_replace(s, trailing, "");
        std::regex numbered(R"(^\d+\.\d*\.?\s*)");
        s = std::regex_replace(s, numbered, "");
        std::regex status(R"(:\s*(OK|ERROR|SUCCESS|FAILED|200|201|400|404|500)(\s*\([^)]*\))?$)");
        s = std::regex_replace(s, status, "");
        return trim(s);
    }

    static bool isValidParticipant(const std::string& s) {
        if (s.size() <= 1) return false;
        if (s.find('-') != std::string::npos || s.find('>') != std::string::npos ||
            s.find('<') != std::string::npos || s.find(">>") != std::string::npos) return false;
        if (s.back() == '.') return false;
        if (std::regex_match(s, std::regex(R"(\d+\.)"))) return false;
        if (std::regex_match(s, std::regex(R"([A-Z]{2,4})"))) return false;
        return true;
    }

    static void extractInteractions(const std::string& text,
                                   const std::map<std::string, std::string>& participants,
                                   std::vector<Interaction>& out) {
        std::vector<std::string> patterns = {
            R"(([^\s]+)\s*-->>\s*([^\s]+)\s*:\s*([^\n]+))",
            R"(([^\s]+)\s*-->\s*([^\s]+)\s*:\s*([^\n]+))",
            R"(([^\s]+)\s*->>\s*([^\s]+)\s*:\s*([^\n]+))",
            R"(([^\s]+)\s*->\s*([^\s]+)\s*:\s*([^\n]+))",
        };

        struct MatchInfo {
            std::smatch m;
            size_t start;
            size_t end;
        };
        std::vector<MatchInfo> allMatches;

        for (const auto& pat : patterns) {
            std::regex re(pat);
            std::sregex_iterator it(text.begin(), text.end(), re);
            for (; it != std::sregex_iterator(); ++it) {
                MatchInfo info;
                info.m = *it;
                info.start = it->position(0);
                info.end = info.start + it->length(0);
                allMatches.push_back(info);
            }
        }

        std::sort(allMatches.begin(), allMatches.end(),
                  [](const MatchInfo& a, const MatchInfo& b) { return a.start < b.start; });

        std::vector<std::pair<size_t, size_t>> processed;
        int step = 0;
        for (const auto& mi : allMatches) {
            bool overlaps = false;
            for (const auto& p : processed) {
                if (!(mi.end <= p.first || mi.start >= p.second)) {
                    overlaps = true;
                    break;
                }
            }
            if (!overlaps) {
                processed.emplace_back(mi.start, mi.end);
                step++;
                std::string src = trim(mi.m[1].str());
                std::string tgt = trim(mi.m[2].str());
                std::string msg = stripHtml(trim(mi.m[3].str()));
                out.push_back({src, tgt, msg, step});
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

    static std::string buildJson(const std::map<std::string, std::string>& participants,
                                 const std::vector<Interaction>& interactions) {
        Poco::JSON::Object root;
        root.set("success", true);

        Poco::JSON::Array compArr;
        for (const auto& p : participants) {
            Poco::JSON::Object c;
            c.set("id", p.first);
            c.set("code", p.first);
            c.set("name", p.second);
            compArr.add(c);
        }
        root.set("components", compArr);

        Poco::JSON::Array reqArr;
        for (const auto& r : interactions) {
            Poco::JSON::Object req;
            req.set("request_id", r.step);
            req.set("component_source_id", r.source);
            req.set("component_target_id", r.target);
            req.set("description", r.message);
            reqArr.add(req);
        }
        root.set("requests", reqArr);

        std::stringstream ss;
        Poco::JSON::Stringifier::stringify(root, ss);
        return ss.str();
    }
};

} // namespace plantuml
