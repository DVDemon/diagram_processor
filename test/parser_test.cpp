#include <gtest/gtest.h>
#include <Poco/JSON/Parser.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Array.h>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>

#include "../src/plantuml/plantuml_sequence.h"
#include "../src/plantuml/plantuml_c4.h"
#include "../src/drawio/drawio_parser.h"
#include "../src/confluence/confluence_parser.h"

#ifdef _WIN32
#define TEST_FIXTURES_BASE TEST_FIXTURES_PATH
#else
#define TEST_FIXTURES_BASE TEST_FIXTURES_PATH
#endif

namespace {

std::string loadFile(const std::string& path) {
    std::ifstream f(path);
    if (!f) return "";
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

Poco::JSON::Object::Ptr parseJson(const std::string& jsonStr) {
    Poco::JSON::Parser parser;
    auto result = parser.parse(jsonStr);
    return result.extract<Poco::JSON::Object::Ptr>();
}

bool jsonEquals(const Poco::JSON::Object::Ptr& actual, const Poco::JSON::Object::Ptr& expected) {
    if (!actual || !expected) return actual == expected;

    // Compare success
    if (expected->has("success")) {
        if (!actual->has("success") || actual->getValue<bool>("success") != expected->getValue<bool>("success"))
            return false;
    }

    // Compare components (order-independent by id)
    if (expected->has("components")) {
        auto expArr = expected->getArray("components");
        auto actArr = actual->getArray("components");
        if (!actArr || expArr->size() != actArr->size()) return false;
        for (size_t i = 0; i < expArr->size(); ++i) {
            auto expObj = expArr->getObject(i);
            std::string expId = expObj->getValue<std::string>("id");
            bool found = false;
            for (size_t j = 0; j < actArr->size(); ++j) {
                auto actObj = actArr->getObject(j);
                if (actObj->getValue<std::string>("id") == expId) {
                    if (actObj->getValue<std::string>("name") != expObj->getValue<std::string>("name"))
                        return false;
                    if (expObj->has("c4_type") && actObj->getValue<std::string>("c4_type") != expObj->getValue<std::string>("c4_type"))
                        return false;
                    found = true;
                    break;
                }
            }
            if (!found) return false;
        }
    }

    // Compare requests (order-independent by source/target/description)
    if (expected->has("requests")) {
        auto expArr = expected->getArray("requests");
        auto actArr = actual->getArray("requests");
        if (!actArr || expArr->size() != actArr->size()) return false;
        for (size_t i = 0; i < expArr->size(); ++i) {
            auto expObj = expArr->getObject(i);
            std::string expSrc = expObj->getValue<std::string>("component_source_id");
            std::string expTgt = expObj->getValue<std::string>("component_target_id");
            std::string expDesc = expObj->getValue<std::string>("description");
            bool found = false;
            for (size_t j = 0; j < actArr->size(); ++j) {
                auto actObj = actArr->getObject(j);
                if (actObj->getValue<std::string>("component_source_id") == expSrc &&
                    actObj->getValue<std::string>("component_target_id") == expTgt &&
                    actObj->getValue<std::string>("description") == expDesc) {
                    found = true;
                    break;
                }
            }
            if (!found) return false;
        }
    }

    // Compare parent_child (order-independent)
    if (expected->has("parent_child")) {
        auto expArr = expected->getArray("parent_child");
        auto actArr = actual->getArray("parent_child");
        if (!actArr || expArr->size() != actArr->size()) return false;
        for (size_t i = 0; i < expArr->size(); ++i) {
            auto expObj = expArr->getObject(i);
            std::string expParent = expObj->getValue<std::string>("parent_id");
            std::string expChild = expObj->getValue<std::string>("child_id");
            bool found = false;
            for (size_t j = 0; j < actArr->size(); ++j) {
                auto actObj = actArr->getObject(j);
                if (actObj->getValue<std::string>("parent_id") == expParent &&
                    actObj->getValue<std::string>("child_id") == expChild) {
                    found = true;
                    break;
                }
            }
            if (!found) return false;
        }
    }

    return true;
}

} // namespace

// ============== PlantUML Sequence ==============
class PlantUmlSequenceTest : public ::testing::TestWithParam<std::pair<std::string, std::string>> {};

TEST_P(PlantUmlSequenceTest, ParseMatchesExpected) {
    auto [inputName, expectedName] = GetParam();
    std::string base(TEST_FIXTURES_BASE);
    std::string inputPath = base + "/plantuml_sequence/" + inputName + ".puml";
    std::string expectedPath = base + "/plantuml_sequence/" + expectedName + ".json";

    std::string input = loadFile(inputPath);
    std::string expectedStr = loadFile(expectedPath);
    ASSERT_FALSE(input.empty()) << "Failed to load: " << inputPath;
    ASSERT_FALSE(expectedStr.empty()) << "Failed to load: " << expectedPath;

    std::string result = plantuml::PlantUmlSequenceParser::parse(input);
    auto actual = parseJson(result);
    auto expected = parseJson(expectedStr);
    ASSERT_TRUE(actual) << "Failed to parse result JSON";
    ASSERT_TRUE(expected) << "Failed to parse expected JSON";

    EXPECT_TRUE(jsonEquals(actual, expected)) << "Input: " << inputName;
}

INSTANTIATE_TEST_SUITE_P(
    PlantUmlSequenceFixtures,
    PlantUmlSequenceTest,
    ::testing::Values(
        std::make_pair("simple_sequence", "simple_sequence"),
        std::make_pair("arrows_only", "arrows_only")
    )
);

// ============== PlantUML C4 ==============
class PlantUmlC4Test : public ::testing::TestWithParam<std::pair<std::string, std::string>> {};

TEST_P(PlantUmlC4Test, ParseMatchesExpected) {
    auto [inputName, expectedName] = GetParam();
    std::string base(TEST_FIXTURES_BASE);
    std::string inputPath = base + "/plantuml_c4/" + inputName + ".puml";
    std::string expectedPath = base + "/plantuml_c4/" + expectedName + ".json";

    std::string input = loadFile(inputPath);
    std::string expectedStr = loadFile(expectedPath);
    ASSERT_FALSE(input.empty()) << "Failed to load: " << inputPath;
    ASSERT_FALSE(expectedStr.empty()) << "Failed to load: " << expectedPath;

    std::string result = plantuml::PlantUmlC4Parser::parse(input);
    auto actual = parseJson(result);
    auto expected = parseJson(expectedStr);
    ASSERT_TRUE(actual) << "Failed to parse result JSON";
    ASSERT_TRUE(expected) << "Failed to parse expected JSON";

    EXPECT_TRUE(jsonEquals(actual, expected)) << "Input: " << inputName;
}

INSTANTIATE_TEST_SUITE_P(
    PlantUmlC4Fixtures,
    PlantUmlC4Test,
    ::testing::Values(
        std::make_pair("rectangle_c4", "rectangle_c4"),
        std::make_pair("c4_lib", "c4_lib")
    )
);

// ============== DrawIO ==============
class DrawioTest : public ::testing::TestWithParam<std::pair<std::string, std::string>> {};

TEST_P(DrawioTest, ParseMatchesExpected) {
    auto [inputName, expectedName] = GetParam();
    std::string base(TEST_FIXTURES_BASE);
    std::string inputPath = base + "/drawio/" + inputName + ".drawio";
    std::string expectedPath = base + "/drawio/" + expectedName + ".json";

    std::string input = loadFile(inputPath);
    std::string expectedStr = loadFile(expectedPath);
    ASSERT_FALSE(input.empty()) << "Failed to load: " << inputPath;
    ASSERT_FALSE(expectedStr.empty()) << "Failed to load: " << expectedPath;

    std::string result = drawio::DrawioParser::parse(input);
    auto actual = parseJson(result);
    auto expected = parseJson(expectedStr);
    ASSERT_TRUE(actual) << "Failed to parse result JSON";
    ASSERT_TRUE(expected) << "Failed to parse expected JSON";

    EXPECT_TRUE(jsonEquals(actual, expected)) << "Input: " << inputName << "\nActual: " << result;
}

INSTANTIATE_TEST_SUITE_P(
    DrawioFixtures,
    DrawioTest,
    ::testing::Values(
        std::make_pair("simple_c4", "simple_c4"),
        std::make_pair("multi_component", "multi_component"),
        std::make_pair("complex", "complex")
    )
);

// ============== Confluence ==============
class ConfluenceTest : public ::testing::TestWithParam<std::pair<std::string, std::string>> {};

TEST_P(ConfluenceTest, ParseMatchesExpected) {
    auto [inputName, expectedName] = GetParam();
    std::string base(TEST_FIXTURES_BASE);
    std::string inputPath = base + "/confluence/" + inputName + ".html";
    std::string expectedPath = base + "/confluence/" + expectedName + ".json";

    std::string input = loadFile(inputPath);
    std::string expectedStr = loadFile(expectedPath);
    ASSERT_FALSE(input.empty()) << "Failed to load: " << inputPath;
    ASSERT_FALSE(expectedStr.empty()) << "Failed to load: " << expectedPath;

    auto diagrams = confluence::ConfluenceParser::parse(input);
    auto expected = parseJson(expectedStr);
    ASSERT_TRUE(expected) << "Failed to parse expected JSON";

    EXPECT_EQ(static_cast<int>(diagrams.size()), expected->getValue<int>("count"))
        << "Input: " << inputName;

    auto expDiagrams = expected->getArray("diagrams");
    ASSERT_EQ(diagrams.size(), expDiagrams->size());
    for (size_t i = 0; i < diagrams.size(); ++i) {
        const auto& d = diagrams[i];
        auto expD = expDiagrams->getObject(i);
        EXPECT_EQ(d.format == confluence::DiagramFormat::PlantUML ? "plantuml" : "drawio",
                  expD->getValue<std::string>("format"))
            << "Diagram " << i;
        EXPECT_EQ(d.subtype, expD->getValue<std::string>("subtype")) << "Diagram " << i;
        EXPECT_EQ(d.sectionTitle, expD->getValue<std::string>("sectionTitle")) << "Diagram " << i;
        if (expD->has("textLength")) {
            EXPECT_EQ(static_cast<int>(d.text.size()), expD->getValue<int>("textLength"))
                << "Diagram " << i << " text length";
        }
    }
}

INSTANTIATE_TEST_SUITE_P(
    ConfluenceFixtures,
    ConfluenceTest,
    ::testing::Values(
        std::make_pair("mixed_diagrams", "mixed_diagrams"),
        std::make_pair("plantuml_only", "plantuml_only")
    )
);
