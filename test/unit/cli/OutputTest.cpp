// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "Output.h"

#include "CliTestSupport.h"
#include <ao/yaml/Reflect.h>
#include <ao/yaml/RymlAdapter.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace ao::cli::test
{
  struct EmitReportDto final
  {
    std::string name{};
    std::uint64_t count = 0;
    bool ok = false;
    std::vector<std::uint64_t> ids{};
    std::optional<std::string> optOmitted{};
  };
} // namespace ao::cli::test

template<>
struct ao::yaml::ReflectNameOverrides<ao::cli::test::EmitReportDto>
{
  static constexpr std::string_view keyFor(std::string_view memberName) noexcept
  {
    if (memberName == "optOmitted")
    {
      return "omitted";
    }

    return memberName;
  }
};

namespace ao::cli::test
{
  TEST_CASE("Output - emitDocument writes reflected YAML documents", "[cli][unit][output]")
  {
    auto os = std::ostringstream{};
    emitDocument(os, OutputFormat::Yaml, EmitReportDto{.name = "alpha", .count = 2, .ok = true, .ids = {4, 8}});

    REQUIRE_FALSE(os.str().empty());
    CHECK(os.str().back() == '\n');

    auto tree = parseYaml(os.str());
    CHECK(yaml::scalarView(tree.rootref()["name"]) == "alpha");
    CHECK(yaml::scalarView(tree.rootref()["count"]) == "2");
    CHECK(yaml::scalarView(tree.rootref()["ok"]) == "true");
    REQUIRE(tree.rootref()["ids"].is_seq());
    CHECK(tree.rootref()["ids"].num_children() == 2);
    CHECK_FALSE(tree.rootref()["omitted"].readable());
  }

  TEST_CASE("Output - emitDocument writes reflected JSON documents", "[cli][unit][output]")
  {
    auto os = std::ostringstream{};
    emitDocument(os, OutputFormat::Json, EmitReportDto{.name = "beta", .count = 3, .ok = true, .ids = {5}});

    REQUIRE_FALSE(os.str().empty());
    CHECK(os.str().back() == '\n');

    auto tree = parseYaml(os.str());
    CHECK(yaml::scalarView(tree.rootref()["name"]) == "beta");
    CHECK(yaml::scalarView(tree.rootref()["count"]) == "3");
    CHECK(yaml::scalarView(tree.rootref()["ids"][0]) == "5");
  }

  TEST_CASE("Output - emitDocument skips plain output", "[cli][unit][output]")
  {
    auto os = std::ostringstream{};
    emitDocument(os, OutputFormat::Plain, EmitReportDto{.name = "plain"});

    CHECK(os.str().empty());
  }
} // namespace ao::cli::test
