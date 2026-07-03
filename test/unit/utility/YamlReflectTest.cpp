// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/utility/EnumName.h>
#include <ao/yaml/Reflect.h>
#include <ao/yaml/Utils.h>

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
  namespace
  {
    enum class FixtureState : std::uint8_t
    {
      Ready,
      Done,
    };

    struct NestedDto final
    {
      std::string name;
      std::int32_t count = 0;
    };

    struct ReflectFixtureDto final
    {
      std::string text;
      std::string emptyText;
      std::string_view view;
      std::int32_t number = 0;
      bool ok = false;
      std::optional<std::string> optOmitted;
      std::optional<std::string> optPresent;
      std::vector<std::string> emptyVector;
      std::vector<NestedDto> nested;
      std::map<std::string, std::string> labels;
      TrackId trackId{};
      ListId listId{};
      std::filesystem::path path;
      FixtureState state = FixtureState::Ready;
      std::uint32_t newCount = 0;
    };

    ryml::Tree parseYaml(std::string_view text)
    {
      auto context = CallbackContext{"<reflect-test>"};
      auto tree = ryml::Tree{callbacks(context)};
      parseInArena(tree, text, context);
      return tree;
    }
  } // namespace
} // namespace ao::yaml::test

template<>
struct ao::yaml::ReflectEnumTraits<ao::yaml::test::FixtureState>
{
  static constexpr auto names()
  {
    return utility::EnumNameTable<test::FixtureState, 2>{{
      {test::FixtureState::Ready, "ready"},
      {test::FixtureState::Done, "done"},
    }};
  }
};

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
  TEST_CASE("Yaml reflect writes reflected DTOs as YAML and JSON", "[core][unit][yaml][reflect]")
  {
    auto dto = ReflectFixtureDto{
      .text = "quote \" slash \\ newline\n tab\t",
      .emptyText = "",
      .view = "123",
      .number = 123,
      .ok = true,
      .optOmitted = std::nullopt,
      .optPresent = "value",
      .emptyVector = {},
      .nested = {NestedDto{.name = "child", .count = 7}},
      .labels = {{"alpha", "one"}, {"empty", ""}},
      .trackId = TrackId{42},
      .listId = ListId{11},
      .path = std::filesystem::path{"folder/file.flac"},
      .state = FixtureState::Done,
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
    CHECK(findChild(root, "emptyVector").is_seq());
    CHECK(findChild(root, "emptyVector").num_children() == 0);
    CHECK(scalarView(findChild(findChild(root, "nested")[0], "name")) == "child");
    CHECK(scalarView(findChild(findChild(root, "labels"), "alpha")) == "one");
    CHECK(scalarView(findChild(root, "trackId")) == "42");
    CHECK(scalarView(findChild(root, "listId")) == "11");
    CHECK(scalarView(findChild(root, "path")) == "folder/file.flac");
    CHECK(scalarView(findChild(root, "state")) == "done");
    CHECK(scalarView(findChild(root, "new")) == "5");

    auto const jsonText = toJsonString(dto);

    CHECK(jsonText.find("\"view\": \"123\"") != std::string::npos);
    CHECK(jsonText.find("\"number\": 123") != std::string::npos);
    CHECK(jsonText.find("\"emptyText\": \"\"") != std::string::npos);
    CHECK(jsonText.find("\"emptyVector\": []") != std::string::npos);
    CHECK(jsonText.find("\"labels\": {") != std::string::npos);
    CHECK(jsonText.find("\"omitted\"") == std::string::npos);
    CHECK(jsonText.find("\"new\": 5") != std::string::npos);
  }
} // namespace ao::yaml::test
