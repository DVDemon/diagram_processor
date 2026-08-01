#pragma once

#include <algorithm>
#include <regex>
#include <string>
#include <vector>

namespace confluence {

enum class DiagramFormat { PlantUML, DrawIO };

struct Diagram {
    std::string text;
    DiagramFormat format;
    std::string subtype;  // "sequence", "c4", "component", etc.
    std::string sectionTitle;
};

/**
 * Parser for Confluence storage format HTML.
 * Extracts PlantUML and DrawIO diagrams from ac:structured-macro elements.
 */
class ConfluenceParser {
public:
    /**
     * Parse Confluence page HTML and extract diagrams.
     * @param html Confluence storage format HTML (body.storage.value)
     * @return Vector of found diagrams with text and type
     */
    static std::vector<Diagram> parse(const std::string& html) {
        std::vector<Diagram> result;
        extractPlantUML(html, result);
        extractDrawIO(html, result);
        return result;
    }

    static std::string detectDrawIOTypePublic(const std::string& xml) {
        return detectDrawIOType(xml);
    }

private:
    static void extractPlantUML(const std::string& html, std::vector<Diagram>& out) {
        std::regex macroRe("<ac:structured-macro\\b[^>]*\\bac:name=\"plantuml\"[^>]*>[\\s\\S]*?</ac:structured-macro>",
                          std::regex::icase);
        std::sregex_iterator it(html.begin(), html.end(), macroRe);
        std::sregex_iterator end;

        for (; it != end; ++it) {
            size_t pos = it->position();
            std::string macro = (*it).str();
            std::string code = extractPlantUMLCode(macro);
            if (!code.empty()) {
                Diagram d;
                d.text = code;
                d.format = DiagramFormat::PlantUML;
                d.subtype = detectPlantUMLType(code);
                d.sectionTitle = getSectionTitleBefore(html, pos);
                out.push_back(d);
            }
        }
    }

    static std::string extractPlantUMLCode(const std::string& macro) {
        std::smatch m;
        if (std::regex_search(macro, m, std::regex("<!\\[CDATA\\[([\\s\\S]*?)\\]\\]>", std::regex::icase))) {
            return trim(m[1].str());
        }
        if (std::regex_search(macro, m, std::regex("<ac:plain-text-body[^>]*>([\\s\\S]*?)</ac:plain-text-body>", std::regex::icase))) {
            return trim(m[1].str());
        }
        return "";
    }

    static std::string detectPlantUMLType(const std::string& code) {
        std::string lower = toLower(code);
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

    static void extractDrawIO(const std::string& html, std::vector<Diagram>& out) {
        for (const char* name : {"drawio", "draw.io", "draw-io"}) {
            std::string pattern = "<ac:structured-macro\\b[^>]*\\bac:name=\"" + std::string(name) + "\"[^>]*>[\\s\\S]*?</ac:structured-macro>";
            std::regex macroRe(pattern, std::regex::icase);
            std::sregex_iterator it(html.begin(), html.end(), macroRe);
            std::sregex_iterator end;

            for (; it != end; ++it) {
                size_t pos = it->position();
                std::string macro = (*it).str();
                std::string xml = extractDrawIOXml(macro);
                std::string meta = extractDrawIOMetadata(macro);
                Diagram d;
                d.text = xml.empty()
                    ? (meta.empty() ? "<!-- DrawIO diagram (attachment) -->" : "<!-- DrawIO: " + meta + " (attachment) -->")
                    : xml;
                d.format = DiagramFormat::DrawIO;
                d.subtype = xml.empty() ? "attachment" : detectDrawIOType(xml);
                d.sectionTitle = getSectionTitleBefore(html, pos);
                out.push_back(d);
            }
        }
    }

    static std::string extractDrawIOMetadata(const std::string& macro) {
        std::smatch m;
        if (std::regex_search(macro, m, std::regex("<ac:parameter\\s+ac:name=\"diagramName\"[^>]*>([^<]*)</ac:parameter>", std::regex::icase))) {
            return trim(m[1].str());
        }
        if (std::regex_search(macro, m, std::regex("<ac:parameter\\s+ac:name=\"diagramDisplayName\"[^>]*>([^<]*)</ac:parameter>", std::regex::icase))) {
            return trim(m[1].str());
        }
        if (std::regex_search(macro, m, std::regex("ri:filename=\"([^\"]+)\""))) {
            return m[1].str();
        }
        return "";
    }

    static std::string extractDrawIOXml(const std::string& macro) {
        std::smatch m;
        if (std::regex_search(macro, m, std::regex("<!\\[CDATA\\[([\\s\\S]*?)\\]\\]>", std::regex::icase))) {
            std::string s = trim(m[1].str());
            if (s.find("<mxfile") != std::string::npos || s.find("<mxGraphModel") != std::string::npos)
                return s;
        }
        if (std::regex_search(macro, m, std::regex("<ac:plain-text-body[^>]*>([\\s\\S]*?)</ac:plain-text-body>", std::regex::icase))) {
            std::string s = trim(m[1].str());
            if (s.find("<mxfile") != std::string::npos || s.find("<mxGraphModel") != std::string::npos)
                return s;
        }
        if (std::regex_search(macro, m, std::regex("<mxfile[^>]*>[\\s\\S]*?</mxfile>", std::regex::icase))) {
            return m[0].str();
        }
        if (std::regex_search(macro, m, std::regex("<mxGraphModel[^>]*>[\\s\\S]*?</mxGraphModel>", std::regex::icase))) {
            return m[0].str();
        }
        return "";
    }

    static std::string detectDrawIOType(const std::string& xml) {
        if (xml.find("c4Type") != std::string::npos) return "c4";
        std::string lower = toLower(xml);
        if (lower.find("uml") != std::string::npos || lower.find("sequence") != std::string::npos) return "uml";
        if (lower.find("flowchart") != std::string::npos || lower.find("flow") != std::string::npos) return "flowchart";
        if (lower.find("architecture") != std::string::npos || lower.find("component") != std::string::npos) return "architecture";
        return "unknown";
    }

    static std::string getSectionTitleBefore(const std::string& html, size_t pos) {
        if (pos == 0) return "";
        std::string before = html.substr(0, pos);
        std::smatch m;
        std::regex h2Re("<h2[^>]*>([^<]*)</h2>", std::regex::icase);
        std::string lastTitle;
        for (auto it = std::sregex_iterator(before.begin(), before.end(), h2Re); it != std::sregex_iterator(); ++it) {
            lastTitle = trim((*it)[1].str());
        }
        if (lastTitle.empty()) {
            std::regex h1Re("<h1[^>]*>([^<]*)</h1>", std::regex::icase);
            for (auto it = std::sregex_iterator(before.begin(), before.end(), h1Re); it != std::sregex_iterator(); ++it) {
                lastTitle = trim((*it)[1].str());
            }
        }
        return lastTitle;
    }

    static std::string trim(const std::string& s) {
        size_t start = s.find_first_not_of(" \t\n\r");
        if (start == std::string::npos) return "";
        size_t end = s.find_last_not_of(" \t\n\r");
        return s.substr(start, end - start + 1);
    }

    static std::string toLower(const std::string& s) {
        std::string r;
        r.reserve(s.size());
        for (char c : s) r += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return r;
    }
};

} // namespace confluence
