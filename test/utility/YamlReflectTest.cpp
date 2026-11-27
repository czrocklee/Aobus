// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/utility/Path.h>
#include <ao/yaml/Reflect.h>
#include <ao/yaml/RymlAdapter.h>

#include <c4/yml/tree.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ao::yaml::test
{
  // NOTE: fixture types deliberately have external linkage (no anonymous
  // namespace): boost.pfr reflection rejects internal-linkage types under
  // MSVC (error C7631).
  // NOLINTBEGIN(misc-use-internal-linkage) — external linkage is required by boost.pfr on MSVC.
  struct NestedDto final
  {
    std::string name;
    std::int32_t count = 0;
  };

  struct ReflectFixtureDto final
  {
    std::string text = {};
    std::string emptyText = {};
    std::string_view view = {};
    std::int32_t number = 0;
    bool ok = false;
    std::optional<std::string> optOmitted = std::nullopt;
    std::optional<std::string> optPresent = std::nullopt;
    std::vector<std::string> emptyNames = {};
    std::vector<NestedDto> nested = {};
    std::map<std::string, std::string> labels;
    TrackId trackId{};
    ListId listId{};
    std::filesystem::path path;
    std::uint32_t newCount = 0;
  };
  // NOLINTEND(misc-use-internal-linkage)

  namespace
  {
    ryml::Tree parseYaml(std::string_view text)
    {
      auto state = ErrorCallbackState{"<reflect-test>"};
      auto tree = ryml::Tree{callbacks()};
      REQUIRE(parseInArena(tree, text, state));
      return tree;
    }
  } // namespace
} // namespace ao::yaml::test

template<>
struct ao::yaml::ReflectNameOverrides<ao::yaml::test::ReflectFixtureDto>
{
  static constexpr std::string_view keyFor(std::string_view memberName) noexcept
  {
    if (memberName == "newCount")
    {
      return "new";
    }

    if (memberName == "optOmitted")
    {
      return "omitted";
    }

    if (memberName == "optPresent")
    {
      return "present";
    }

    return memberName;
  }
};

namespace ao::yaml::test
{
  TEST_CASE("YamlReflect - writes reflected DTOs as YAML and JSON")
  {
    auto dto = ReflectFixtureDto{
      .text = "quote \" slash \\ newline\n tab\t",
      .emptyText = "",
      .view = "123",
      .number = 123,
      .ok = true,
      .optOmitted = std::nullopt,
      .optPresent = "value",
      .nested = {NestedDto{.name = "child", .count = 7}},
      .labels = {{"alpha", "one"}, {"empty", ""}},
      .trackId = TrackId{42},
      .listId = ListId{11},
      .path = utility::pathFromUtf8("folder/Dvo\xC5\x99\xC3\xA1k.flac"),
      .newCount = 5,
    };

    auto const yamlText = toYamlString(dto);
    auto tree = parseYaml(yamlText);
    auto root = tree.rootref();

    CHECK(scalarView(findChild(root, "text")) == dto.text);
    CHECK(scalarView(findChild(root, "emptyText")).empty());
    CHECK(scalarView(findChild(root, "view")) == "123");
    CHECK(findChild(root, "omitted").readable() == false);
    CHECK(scalarView(findChild(root, "present")) == "value");
    CHECK(findChild(root, "emptyNames").is_seq());
    CHECK(findChild(root, "emptyNames").num_children() == 0);
    CHECK(scalarView(findChild(findChild(root, "nested")[0], "name")) == "child");
    CHECK(scalarView(findChild(findChild(root, "labels"), "alpha")) == "one");
    CHECK(scalarView(findChild(root, "trackId")) == "42");
    CHECK(scalarView(findChild(root, "listId")) == "11");
    CHECK(scalarView(findChild(root, "path")) == "folder/Dvo\xC5\x99\xC3\xA1k.flac");
    CHECK(scalarView(findChild(root, "new")) == "5");

    auto const jsonText = toJsonString(dto);

    CHECK(jsonText.contains("\"view\": \"123\""));
    CHECK(jsonText.contains("\"number\": 123"));
    CHECK(jsonText.contains("\"emptyText\": \"\""));
    CHECK(jsonText.contains("\"emptyNames\": []"));
    CHECK(jsonText.contains("\"labels\": {"));
    CHECK_FALSE(jsonText.contains("\"omitted\""));
    CHECK(jsonText.contains("\"new\": 5"));
  }

  TEST_CASE("YamlReflect - JSON map keys and values distinguish NUL from literal backslash-zero")
  {
    constexpr auto kNulKey = std::string_view{"key\0end", 7};
    constexpr auto kNulValue = std::string_view{"value\0end", 9};
    constexpr auto kLiteralKey = std::string_view{R"(key\0end)"};
    constexpr auto kLiteralValue = std::string_view{R"(value\0end)"};
    auto dto = ReflectFixtureDto{};
    dto.labels.emplace(kNulKey, kNulValue);
    dto.labels.emplace(kLiteralKey, kLiteralValue);

    auto const jsonText = toJsonString(dto);
    CHECK_FALSE(jsonText.contains('\0'));
    CHECK(jsonText.contains(R"("key\u0000end")"));
    CHECK(jsonText.contains(R"("value\u0000end")"));

    auto parsed = parseYaml(jsonText);
    auto const labels = findChild(parsed.rootref(), "labels");
    REQUIRE(labels.readable());
    auto const nulEntry = findChild(labels, kNulKey);
    auto const literalEntry = findChild(labels, kLiteralKey);
    REQUIRE(nulEntry.readable());
    REQUIRE(literalEntry.readable());
    CHECK(scalarView(nulEntry) == kNulValue);
    CHECK(scalarView(literalEntry) == kLiteralValue);
  }
} // namespace ao::yaml::test
