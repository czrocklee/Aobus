// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/unit/TestUtils.h"
#include <ao/Exception.h>
#include <ao/yaml/RymlAdapter.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace ao::test
{
  namespace
  {
    ryml::Tree parseYaml(std::string_view text)
    {
      auto context = yaml::CallbackContext{};
      auto tree = ryml::Tree{yaml::callbacks(context)};
      yaml::parseInArena(tree, text, context);
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

    SECTION("scalarAs reports malformed scalars as FormatRejected")
    {
      auto tree = parseYaml("3.14px");

      auto result = yaml::scalarAs<double>(tree.rootref(), "duration");
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::FormatRejected);
      CHECK(result.error().message.find("duration") != std::string::npos);
    }
  }

  TEST_CASE("RymlAdapter - callback context owns diagnostic filename", "[core][unit][yaml]")
  {
    auto context = yaml::CallbackContext{"fixture.yaml"};
    auto tree = ryml::Tree{yaml::callbacks(context)};

    try
    {
      yaml::parseInArena(tree, "root: [unterminated", context);
      FAIL("invalid YAML should throw through the ryml callback");
    }
    catch (Exception const& e)
    {
      CHECK(std::string_view{e.what()}.find("fixture.yaml") != std::string_view::npos);
    }
  }
} // namespace ao::test
