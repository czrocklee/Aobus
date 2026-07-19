// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/unit/TestUtils.h"
#include <ao/Exception.h>
#include <ao/yaml/RymlAdapter.h>

#include <catch2/catch_test_macros.hpp>
#include <ryml.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>

namespace ao::test
{
  namespace
  {
    ryml::Tree parseYaml(std::string_view text)
    {
      auto state = yaml::ErrorCallbackState{};
      auto tree = ryml::Tree{yaml::callbacks(state)};
      yaml::parseInArena(tree, text, state);
      tree.callbacks(yaml::callbacks());
      return tree;
    }
  } // namespace

  TEST_CASE("RymlAdapter - scalar parsing is strict and non-throwing", "[core][unit][yaml]")
  {
    SECTION("integers must consume the full scalar")
    {
      auto tree = parseYaml("99px");
      std::int32_t value = 7;

      CHECK_FALSE(yaml::tryReadScalar(tree.rootref(), value));
      CHECK(value == 7);
      CHECK(yaml::asInt<int>(tree.rootref(), 7) == 7);
    }

    SECTION("integer range is checked")
    {
      auto tree = parseYaml("256");
      std::uint8_t value = 42;

      CHECK_FALSE(yaml::tryReadScalar(tree.rootref(), value));
      CHECK(value == 42);
    }

    SECTION("unsigned integers reject negative text")
    {
      auto tree = parseYaml("-1");
      std::uint32_t value = 42;

      CHECK_FALSE(yaml::tryReadScalar(tree.rootref(), value));
      CHECK(value == 42);
    }

    SECTION("booleans use canonical YAML text")
    {
      auto trueTree = parseYaml("true");
      auto falseTree = parseYaml("false");
      auto numericTree = parseYaml("1");
      bool value = false;

      REQUIRE(yaml::tryReadScalar(trueTree.rootref(), value));
      CHECK(value == true);

      REQUIRE(yaml::tryReadScalar(falseTree.rootref(), value));
      CHECK(value == false);

      value = true;
      CHECK_FALSE(yaml::tryReadScalar(numericTree.rootref(), value));
      CHECK(value == true);
    }
  }

  TEST_CASE("RymlAdapter - recoverable helpers return Result", "[core][unit][yaml]")
  {
    SECTION("readFileResult reports missing files as IoError")
    {
      auto const tempDir = TempDir{};
      auto const missing = std::filesystem::path{tempDir.path()} / "missing.yaml";

      auto result = yaml::readFileResult(missing);
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::IoError);
    }

    SECTION("readFileResult applies its optional byte ceiling before reading")
    {
      auto const tempDir = TempDir{};
      auto const path = std::filesystem::path{tempDir.path()} / "bounded.yaml";
      std::ofstream{path, std::ios::binary} << "12345";

      auto exact = yaml::readFileResult(path, 5);
      REQUIRE(exact);
      CHECK(exact->size() == 5);

      auto const rejected = yaml::readFileResult(path, 4);
      REQUIRE_FALSE(rejected);
      CHECK(rejected.error().code == Error::Code::ValueTooLarge);
    }

    SECTION("scalarAs reports malformed scalars as FormatRejected")
    {
      auto tree = parseYaml("3.14px");

      auto result = yaml::scalarAs<double>(tree.rootref(), "duration");
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::FormatRejected);
      CHECK(result.error().message.contains("duration"));
    }

    SECTION("null is not accepted as a string scalar")
    {
      auto tree = parseYaml("null");
      auto result = yaml::scalarAs<std::string>(tree.rootref(), "identifier");

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::FormatRejected);
    }

    SECTION("diagnostic context is bounded")
    {
      auto const bounded = yaml::boundedErrorContext(std::string(500, 'x'));
      CHECK(bounded.size() == yaml::kMaximumErrorContextBytes);
      CHECK(bounded.ends_with("..."));
    }
  }

  TEST_CASE("RymlAdapter - error callback state owns diagnostic filename", "[core][unit][yaml]")
  {
    auto state = yaml::ErrorCallbackState{"fixture.yaml"};
    auto tree = ryml::Tree{yaml::callbacks(state)};

    try
    {
      yaml::parseInArena(tree, "root: [unterminated", state);
      FAIL("invalid YAML should throw through the ryml callback");
    }
    catch (Exception const& e)
    {
      CHECK(std::string_view{e.what()}.contains("fixture.yaml"));
    }
  }
} // namespace ao::test
