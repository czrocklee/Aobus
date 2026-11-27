// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/yaml/RymlAdapter.h>

#include "test/TestFixtureSupport.h"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <ryml.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace ao::test
{
  namespace
  {
    ryml::Tree parseYaml(std::string_view text)
    {
      auto state = yaml::ErrorCallbackState{};
      auto tree = ryml::Tree{yaml::callbacks()};
      REQUIRE(yaml::parseInArena(tree, text, state));
      return tree;
    }
  } // namespace

  TEST_CASE("RymlAdapter - scalar parsing is strict and non-throwing")
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

  TEST_CASE("RymlAdapter - integer parsing accepts endpoints and preserves destinations on overflow")
  {
    SECTION("signed 32-bit bounds")
    {
      constexpr std::int32_t kSeed = 42;
      auto value = kSeed;
      REQUIRE(yaml::tryParseScalar("-2147483648", value));
      CHECK(value == std::numeric_limits<std::int32_t>::min());
      REQUIRE(yaml::tryParseScalar("2147483647", value));
      CHECK(value == std::numeric_limits<std::int32_t>::max());

      for (auto const* text : {"-2147483649", "2147483648"})
      {
        INFO(text);
        value = kSeed;
        CHECK_FALSE(yaml::tryParseScalar(text, value));
        CHECK(value == kSeed);
      }
    }

    SECTION("signed 64-bit bounds")
    {
      constexpr std::int64_t kSeed = 42;
      auto value = kSeed;
      REQUIRE(yaml::tryParseScalar("-9223372036854775808", value));
      CHECK(value == std::numeric_limits<std::int64_t>::min());
      REQUIRE(yaml::tryParseScalar("9223372036854775807", value));
      CHECK(value == std::numeric_limits<std::int64_t>::max());

      for (auto const* text : {"-9223372036854775809", "9223372036854775808"})
      {
        INFO(text);
        value = kSeed;
        CHECK_FALSE(yaml::tryParseScalar(text, value));
        CHECK(value == kSeed);
      }
    }

    SECTION("unsigned 64-bit bounds")
    {
      constexpr std::uint64_t kSeed = 42;
      auto value = kSeed;
      REQUIRE(yaml::tryParseScalar("0", value));
      CHECK(value == 0);
      REQUIRE(yaml::tryParseScalar("18446744073709551615", value));
      CHECK(value == std::numeric_limits<std::uint64_t>::max());

      value = kSeed;
      CHECK_FALSE(yaml::tryParseScalar("18446744073709551616", value));
      CHECK(value == kSeed);
    }
  }

  TEST_CASE("RymlAdapter - readBoolOr reads exact boolean text or preserves fallback")
  {
    auto trueTree = parseYaml("true");
    auto falseTree = parseYaml("false");
    CHECK(yaml::readBoolOr(trueTree.rootref(), false));
    CHECK_FALSE(yaml::readBoolOr(falseTree.rootref(), true));

    for (auto const* text : {"1", "0", "TRUE", "unknown", "null", "[true]"})
    {
      INFO(text);
      auto tree = parseYaml(text);
      CHECK_FALSE(yaml::readBoolOr(tree.rootref()));
      CHECK(yaml::readBoolOr(tree.rootref(), true));
    }

    auto tree = parseYaml("key: true");
    CHECK_FALSE(yaml::readBoolOr(tree.rootref(), false));
    CHECK(yaml::readBoolOr(tree.rootref(), true));
  }

  TEST_CASE("RymlAdapter - recoverable helpers return Result")
  {
    SECTION("readFileResult reports missing files as IoError")
    {
      auto const tempDir = TempDir{};
      auto const missing = std::filesystem::path{tempDir.path()} / "missing.yaml";

      auto res = yaml::readFileResult(missing);
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::IoError);
    }

    SECTION("readFileResult rejects directories before reading")
    {
      auto const tempDir = TempDir{};
      auto const directory = std::filesystem::path{tempDir.path()} / "config.yaml";
      REQUIRE(std::filesystem::create_directory(directory));

      auto const res = yaml::readFileResult(directory);
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::IoError);
    }

    SECTION("readFileResult applies its optional byte ceiling before reading")
    {
      auto const tempDir = TempDir{};
      auto const path = std::filesystem::path{tempDir.path()} / "bounded.yaml";
      std::ofstream{path, std::ios::binary} << "12345";

      auto exactRes = yaml::readFileResult(path, 5);
      REQUIRE(exactRes);
      CHECK(std::string{exactRes->begin(), exactRes->end()} == "12345");

      auto const rejectedRes = yaml::readFileResult(path, 4);
      REQUIRE_FALSE(rejectedRes);
      CHECK(rejectedRes.error().code == Error::Code::ValueTooLarge);

      auto const emptyPath = tempDir.path() / "empty.yaml";
      {
        auto emptyOutput = std::ofstream{emptyPath, std::ios::binary};
        REQUIRE(emptyOutput);
      }

      auto const emptyRes = yaml::readFileResult(emptyPath, 0);
      REQUIRE(emptyRes);
      CHECK(emptyRes->empty());
    }

    SECTION("scalarAs reports malformed scalars as FormatRejected")
    {
      auto tree = parseYaml("3.14px");

      auto res = yaml::scalarAs<double>(tree.rootref(), "duration");
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::FormatRejected);
      CHECK(res.error().message.contains("duration"));
    }

    SECTION("null is not accepted as a string scalar")
    {
      auto tree = parseYaml("null");
      auto res = yaml::scalarAs<std::string>(tree.rootref(), "identifier");

      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::FormatRejected);
    }

    SECTION("diagnostic context is bounded")
    {
      auto const bounded = yaml::boundedErrorContext(std::string(500, 'x'));
      CHECK(bounded.size() == yaml::kMaximumErrorContextBytes);
      CHECK(bounded.ends_with("..."));
    }

    SECTION("bounded diagnostic context ends between characters")
    {
      // The two bytes of U+00E9 straddle the cut that ASCII text would take.
      auto const kept = yaml::kMaximumErrorContextBytes - std::string_view{"..."}.size() - 1;
      auto const context = std::string(kept, 'x') + "\xC3\xA9" + std::string(500, 'x');

      CHECK(yaml::boundedErrorContext(context) == std::string(kept, 'x') + "...");
    }
  }

  TEST_CASE("RymlAdapter - arena parsing retains source bytes after input destruction")
  {
    auto state = yaml::ErrorCallbackState{"arena.yaml"};
    auto tree = ryml::Tree{yaml::callbacks()};
    {
      auto source = std::string{"name: preserved\n"};
      REQUIRE(yaml::parseInArena(tree, source, state));
      CHECK(tree.callbacks() == yaml::callbacks());
      source.assign(source.size(), '#');
    }

    auto root = tree.rootref();
    CHECK(yaml::scalarView(yaml::findChild(root, "name")) == "preserved");
  }

  TEST_CASE("RymlAdapter - error callback state owns diagnostic filename")
  {
    auto state = yaml::ErrorCallbackState{"fixture.yaml"};
    auto tree = ryml::Tree{yaml::callbacks()};
    auto const parsedRes = yaml::parseInArena(tree, "root: [unterminated", state);

    REQUIRE_FALSE(parsedRes);
    CHECK(parsedRes.error().code == Error::Code::FormatRejected);
    CHECK(parsedRes.error().message.contains("fixture.yaml"));
    CHECK(tree.callbacks() == yaml::callbacks());
  }

  TEST_CASE("RymlAdapter - malformed in-place parsing restores fail-fast callbacks")
  {
    constexpr auto kSource = std::string_view{"root: [unterminated"};
    auto buffer = std::vector<char>{kSource.begin(), kSource.end()};
    auto state = yaml::ErrorCallbackState{"malformed-in-place.yaml"};
    auto tree = ryml::Tree{yaml::callbacks()};
    auto const parsedRes = yaml::parseInPlace(tree, buffer, state);

    REQUIRE_FALSE(parsedRes);
    CHECK(parsedRes.error().code == Error::Code::FormatRejected);
    CHECK(parsedRes.error().message.contains("malformed-in-place.yaml"));
    CHECK(tree.callbacks() == yaml::callbacks());
  }

  TEST_CASE("RymlAdapter - unresolved alias restores fail-fast callbacks")
  {
    auto state = yaml::ErrorCallbackState{"unresolved-alias.yaml"};
    auto tree = ryml::Tree{yaml::callbacks()};
    REQUIRE(yaml::parseInArena(tree, "item: *missing\n", state));

    auto const resolvedRes = yaml::resolve(tree, state);

    REQUIRE_FALSE(resolvedRes);
    CHECK(resolvedRes.error().code == Error::Code::FormatRejected);
    CHECK(resolvedRes.error().message.contains("unresolved-alias.yaml"));
    CHECK(tree.callbacks() == yaml::callbacks());
  }
} // namespace ao::test
