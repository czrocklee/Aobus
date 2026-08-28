// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/library/presentation/ListPresentationPreferenceYamlSchema.h>

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/uimodel/library/presentation/ListPresentations.h>
#include <ao/yaml/RymlAdapter.h>

#include <catch2/catch_test_macros.hpp>

namespace ao::uimodel::test
{
  TEST_CASE("ListPresentationPreferenceYamlSchema - round-trip preserves opaque presentation ids",
            "[uimodel][unit][presentation-preference]")
  {
    auto state = ListPresentations::Snapshot{};
    state[ListId{10}] = "plugin-preset";

    auto const documentRes = toListPresentationPreferenceDocument(state);

    REQUIRE(documentRes);
    CHECK(documentRes->version == 1);
    REQUIRE(documentRes->preferences.size() == 1);
    CHECK(documentRes->preferences[0].listId == 10);
    CHECK(documentRes->preferences[0].presentationId == "plugin-preset");

    auto const decodedRes = listPresentationPreferenceStateFromDocument(*documentRes);

    REQUIRE(decodedRes);
    REQUIRE((*decodedRes).size() == 1);
    CHECK((*decodedRes).at(ListId{10}) == "plugin-preset");
  }

  TEST_CASE("ListPresentationPreferenceYamlSchema - rejects an invalid document as one object",
            "[uimodel][unit][presentation-preference]")
  {
    auto document = ListPresentationPreferenceDocument{
      .preferences =
        {
          StoredListPresentationPreference{.listId = 10, .presentationId = "albums"},
        },
    };

    SECTION("Unsupported version")
    {
      document.version = 2;
      auto const result = listPresentationPreferenceStateFromDocument(document);

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::NotSupported);
    }

    SECTION("Invalid list id")
    {
      document.preferences[0].listId = kInvalidListId.raw();
      auto const result = listPresentationPreferenceStateFromDocument(document);

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::FormatRejected);
    }

    SECTION("Empty presentation id")
    {
      document.preferences[0].presentationId.clear();
      auto const result = listPresentationPreferenceStateFromDocument(document);

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::FormatRejected);
    }

    SECTION("Duplicate list id")
    {
      document.preferences.push_back(document.preferences[0]);
      auto const result = listPresentationPreferenceStateFromDocument(document);

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::FormatRejected);
    }
  }

  TEST_CASE("ListPresentationPreferenceYamlSchema - owns the exact YAML mapping",
            "[uimodel][unit][presentation-preference][yaml]")
  {
    auto state = ListPresentations::Snapshot{};
    state[ListId{10}] = "plugin-preset";
    auto tree = ryml::Tree{yaml::callbacks()};

    REQUIRE(ListPresentationPreferenceYamlSchema{}.serialize(tree.rootref(), state));
    CHECK(yaml::scalarView(tree.rootref()["version"]) == "1");
    REQUIRE(tree.rootref()["preferences"].is_seq());
    CHECK(yaml::scalarView(tree.rootref()["preferences"][0]["presentationId"]) == "plugin-preset");

    auto const decodedRes =
      ListPresentationPreferenceYamlSchema{}.deserialize(tree.rootref(), ListPresentations::Snapshot{});
    REQUIRE(decodedRes);
    CHECK((*decodedRes) == state);
  }

  TEST_CASE("ListPresentationPreferenceYamlSchema - rejects invalid YAML candidates",
            "[uimodel][unit][presentation-preference][yaml]")
  {
    SECTION("Future version is reported before interpreting its payload")
    {
      auto const* source = "version: 99\npreferences: malformed\nfuture: true\n";
      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(source), &tree);
      auto const decodedRes =
        ListPresentationPreferenceYamlSchema{}.deserialize(tree.rootref(), ListPresentations::Snapshot{});

      REQUIRE_FALSE(decodedRes);
      CHECK(decodedRes.error().code == Error::Code::NotSupported);
    }

    SECTION("Missing required fields are rejected")
    {
      auto const* source = "version: 1\n";
      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(source), &tree);
      auto const decodedRes =
        ListPresentationPreferenceYamlSchema{}.deserialize(tree.rootref(), ListPresentations::Snapshot{});

      REQUIRE_FALSE(decodedRes);
      CHECK(decodedRes.error().code == Error::Code::FormatRejected);
      CHECK(decodedRes.error().message.contains("preferences"));
    }

    SECTION("Unknown structural keys are rejected")
    {
      auto const* source = "version: 1\npreferences: []\nfuture: true\n";
      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(source), &tree);
      auto const decodedRes =
        ListPresentationPreferenceYamlSchema{}.deserialize(tree.rootref(), ListPresentations::Snapshot{});

      REQUIRE_FALSE(decodedRes);
      CHECK(decodedRes.error().code == Error::Code::FormatRejected);
      CHECK(decodedRes.error().message.contains("future"));
    }

    SECTION("Malformed nested entries reject the whole candidate")
    {
      auto const* source = R"(
        version: 1
        preferences:
          - listId: malformed
            presentationId: albums
      )";
      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(source), &tree);
      auto const decodedRes =
        ListPresentationPreferenceYamlSchema{}.deserialize(tree.rootref(), ListPresentations::Snapshot{});

      REQUIRE_FALSE(decodedRes);
      CHECK(decodedRes.error().code == Error::Code::FormatRejected);
      CHECK(decodedRes.error().message.contains("listId"));
    }
  }
} // namespace ao::uimodel::test
