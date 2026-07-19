// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/Error.h>
#include <ao/yaml/RymlAdapter.h>
#include <ao/yaml/Serialization.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <ryml.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

  TEST_CASE("YamlSerialization - node-shape and map-key validation is explicit", "[core][unit][yaml]")
  {
    constexpr auto kAllowed = std::to_array<std::string_view>({"known"});

    SECTION("required node kinds are checked")
    {
      auto scalar = parseYaml("value");
      auto sequence = parseYaml("[one, two]");

      CHECK_FALSE(yaml::requireMap(scalar.rootref(), "root"));
      CHECK_FALSE(yaml::requireSequence(scalar.rootref(), "root"));
      CHECK(yaml::requireSequence(sequence.rootref(), "root"));
    }

    SECTION("unknown keys follow the caller policy")
    {
      auto tree = parseYaml("known: 1\nfuture: 2\n");
      auto rejected = yaml::validateMapKeys(tree.rootref(), kAllowed, "root");
      auto allowed = yaml::validateMapKeys(tree.rootref(), kAllowed, "root", yaml::UnknownKeyPolicy::Allow);

      REQUIRE_FALSE(rejected);
      CHECK(rejected.error().message.contains("future"));
      CHECK(allowed);
    }

    SECTION("duplicate keys are rejected under both unknown-key policies")
    {
      auto tree = parseYaml("known: 1\nknown: 2\n");
      auto rejected = yaml::validateMapKeys(tree.rootref(), kAllowed, "root");
      auto allowed = yaml::validateMapKeys(tree.rootref(), kAllowed, "root", yaml::UnknownKeyPolicy::Allow);

      REQUIRE_FALSE(rejected);
      REQUIRE_FALSE(allowed);
      CHECK(rejected.error().message.contains("more than once"));
    }
  }

  TEST_CASE("YamlSerialization - MapReader preserves explicit field policy", "[core][unit][yaml]")
  {
    constexpr auto kKeys = std::to_array<std::string_view>({"count", "name"});

    SECTION("required and present optional scalars are assigned")
    {
      auto tree = parseYaml("count: 7\nname: updated\nfuture: retained\n");
      std::int32_t count = 0;
      auto name = std::string{"seed"};
      auto reader = yaml::MapReader{tree.rootref(), kKeys, "state", yaml::UnknownKeyPolicy::Allow};
      reader.requiredScalar("count", count).optionalScalar("name", name);

      auto readCount = std::move(reader).finish(count);
      REQUIRE(readCount);
      CHECK(*readCount == 7);
      CHECK(name == "updated");
    }

    SECTION("missing optional scalars preserve the seed")
    {
      auto tree = parseYaml("count: 7\n");
      std::int32_t count = 0;
      auto name = std::string{"seed"};
      auto reader = yaml::MapReader{tree.rootref(), kKeys, "state"};
      reader.requiredScalar("count", count).optionalScalar("name", name);

      REQUIRE(reader.result());
      CHECK(count == 7);
      CHECK(name == "seed");
    }

    SECTION("present null optional strings are malformed and preserve the seed")
    {
      auto tree = parseYaml("name:\n");
      auto name = std::string{"seed"};
      auto reader = yaml::MapReader{tree.rootref(), kKeys, "state"};
      reader.optionalScalar("name", name);

      REQUIRE_FALSE(reader.result());
      CHECK(reader.result().error().code == Error::Code::FormatRejected);
      CHECK(reader.result().error().message.contains("state.name"));
      CHECK(name == "seed");
    }

    SECTION("the first malformed field is retained with context")
    {
      auto tree = parseYaml("count: invalid\nname: updated\n");
      std::int32_t count = 0;
      auto name = std::string{"seed"};
      auto reader = yaml::MapReader{tree.rootref(), kKeys, "state"};
      reader.requiredScalar("count", count).optionalScalar("name", name);

      REQUIRE_FALSE(reader.result());
      CHECK(reader.result().error().message.contains("state.count"));
      CHECK(name == "seed");
    }

    SECTION("nested values and sequences use the same first-failure policy")
    {
      constexpr auto kNestedKeys = std::to_array<std::string_view>({"nested", "values"});
      auto tree = parseYaml("nested: 9\nvalues: [1, 2, 3]\n");
      std::int32_t nested = 0;
      auto values = std::vector<std::int32_t>{};
      auto reader = yaml::MapReader{tree.rootref(), kNestedKeys, "state"};
      reader
        .requiredValue("nested",
                       nested,
                       [](ryml::ConstNodeRef child, std::string_view context)
                       { return yaml::scalarAs<std::int32_t>(child, context); })
        .requiredScalarSequence("values", values);

      auto read = std::move(reader).finish(std::pair{nested, values});
      REQUIRE(read);
      CHECK(read->first == 9);
      CHECK((read->second == std::vector<std::int32_t>{1, 2, 3}));
    }
  }

  TEST_CASE("YamlSerialization - sequence helpers preserve index context", "[core][unit][yaml]")
  {
    auto tree = parseYaml("[1, invalid, 3]");
    auto read = yaml::readScalarSequence<std::int32_t>(tree.rootref(), "values");

    REQUIRE_FALSE(read);
    CHECK(read.error().code == Error::Code::FormatRejected);
    CHECK(read.error().message.contains("values.1"));
  }

  TEST_CASE("YamlSerialization - emitted scalars and composed maps own their text", "[core][unit][yaml]")
  {
    SECTION("field context is bounded")
    {
      auto const field = yaml::fieldContext(std::string(500, 'x'), std::string(500, 'y'));
      CHECK(field.size() == yaml::kMaximumErrorContextBytes);
      CHECK(field.ends_with("..."));
    }

    SECTION("writeScalar copies transient text into the tree arena")
    {
      auto tree = ryml::Tree{yaml::callbacks()};

      {
        auto transient = std::string{"owned after scope"};
        yaml::writeScalar(tree.rootref(), transient);
      }

      CHECK(ryml::emitrs_yaml<std::string>(tree).contains("owned after scope"));
    }

    SECTION("writeScalar preserves strings that resemble YAML scalar types")
    {
      for (auto const value : {std::string_view{"null"}, std::string_view{"true"}, std::string_view{"42"}})
      {
        CAPTURE(value);
        auto tree = ryml::Tree{yaml::callbacks()};
        yaml::writeScalar(tree.rootref(), value);
        REQUIRE(tree.rootref().is_val_quoted());

        auto parsed = parseYaml(ryml::emitrs_yaml<std::string>(tree));
        auto const read = yaml::scalarAs<std::string>(parsed.rootref(), "string value");

        REQUIRE(read);
        CHECK(*read == value);
      }
    }

    SECTION("MapWriter creates arena-owned scalar fields")
    {
      auto tree = ryml::Tree{yaml::callbacks()};
      auto writer = yaml::MapWriter{tree.rootref()};

      {
        auto transient = std::string{"owned field"};
        writer.scalar("name", transient).scalar("count", 7);
      }

      auto parsed = parseYaml(ryml::emitrs_yaml<std::string>(tree));
      auto name = yaml::scalarAs<std::string>(yaml::findChild(parsed.rootref(), "name"), "name");
      auto count = yaml::scalarAs<std::int32_t>(yaml::findChild(parsed.rootref(), "count"), "count");

      REQUIRE(name);
      REQUIRE(count);
      CHECK(*name == "owned field");
      CHECK(*count == 7);
    }

    SECTION("MapWriter composes nested values and retains the first writer failure")
    {
      auto tree = ryml::Tree{yaml::callbacks()};
      auto writer = yaml::MapWriter{tree.rootref()};
      writer.scalarSequence("values", std::vector{1, 2})
        .value("nested",
               7,
               [](ryml::NodeRef child, std::int32_t value) -> Result<>
               {
                 auto nested = yaml::MapWriter{child};
                 nested.scalar("value", value);
                 return std::move(nested).finish();
               })
        .value("broken",
               0,
               [](ryml::NodeRef /*child*/, std::int32_t /*value*/) -> Result<>
               { return makeError(Error::Code::InvalidState, "intentional writer failure"); })
        .scalar("skipped", true);

      auto result = std::move(writer).finish();
      REQUIRE_FALSE(result);
      CHECK_FALSE(yaml::findChild(tree.rootref(), "skipped").readable());
      CHECK(yaml::findChild(tree.rootref(), "nested").is_map());
    }

    SECTION("string-map helpers preserve dynamic keys")
    {
      using StringIntMap = std::map<std::string, std::int32_t, std::less<>>;
      auto const source = StringIntMap{{"first", 1}, {"second", 2}};
      auto tree = ryml::Tree{yaml::callbacks()};
      auto written = yaml::writeStringMap(tree.rootref(),
                                          source,
                                          "values",
                                          [](ryml::NodeRef child, std::int32_t value) -> Result<>
                                          {
                                            yaml::writeScalar(child, value);
                                            return {};
                                          });
      REQUIRE(written);

      auto read = yaml::readStringMap<StringIntMap>(tree.rootref(),
                                                    "values",
                                                    [](ryml::ConstNodeRef child, std::string_view context)
                                                    { return yaml::scalarAs<std::int32_t>(child, context); });
      REQUIRE(read);
      CHECK(*read == source);
    }

    SECTION("string-map empty keys identify the failing boundary")
    {
      using StringIntMap = std::map<std::string, std::int32_t, std::less<>>;

      auto writtenTree = ryml::Tree{yaml::callbacks()};
      auto const written = yaml::writeStringMap(writtenTree.rootref(),
                                                StringIntMap{{"", 1}},
                                                "values",
                                                [](ryml::NodeRef child, std::int32_t value) -> Result<>
                                                {
                                                  yaml::writeScalar(child, value);
                                                  return {};
                                                });

      REQUIRE_FALSE(written);
      CHECK(written.error().code == Error::Code::InvalidState);

      auto readTree = parseYaml("\"\": 1\n");
      auto const read = yaml::readStringMap<StringIntMap>(readTree.rootref(),
                                                          "values",
                                                          [](ryml::ConstNodeRef child, std::string_view context)
                                                          { return yaml::scalarAs<std::int32_t>(child, context); });

      REQUIRE_FALSE(read);
      CHECK(read.error().code == Error::Code::FormatRejected);
    }
  }
} // namespace ao::test
