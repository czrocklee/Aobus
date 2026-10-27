// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/library/presentation/ListPresentationPreferenceYamlSchema.h>

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/uimodel/library/presentation/ListPresentations.h>
#include <ao/utility/Uuid.h>
#include <ao/yaml/RymlAdapter.h>

#include <catch2/catch_test_macros.hpp>

namespace ao::uimodel::test
{
  namespace
  {
    auto const kLibraryId = utility::parseUuid("123e4567-e89b-12d3-a456-426614174000").value();
  } // namespace

  TEST_CASE("ListPresentationPreferenceYamlSchema - round-trip preserves opaque presentation ids",
            "[uimodel][unit][presentation-preference]")
  {
    auto state = ListPresentations::Snapshot{};
    state[ListId{10}] = "plugin-preset";

    auto const documentRes = toListPresentationPreferenceDocument(state, kLibraryId);

    REQUIRE(documentRes);
    CHECK(documentRes->version == 1);
    CHECK(documentRes->libraryId == kLibraryId);
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
      auto const res = listPresentationPreferenceStateFromDocument(document);

      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::NotSupported);
    }

    SECTION("Invalid list id")
    {
      document.preferences[0].listId = kInvalidListId.raw();
      auto const res = listPresentationPreferenceStateFromDocument(document);

      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::FormatRejected);
    }

    SECTION("Empty presentation id")
    {
      document.preferences[0].presentationId.clear();
      auto const res = listPresentationPreferenceStateFromDocument(document);

      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::FormatRejected);
    }

    SECTION("Duplicate list id")
    {
      document.preferences.push_back(document.preferences[0]);
      auto const res = listPresentationPreferenceStateFromDocument(document);

      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::FormatRejected);
    }
  }

  TEST_CASE("ListPresentationPreferenceYamlSchema - owns the exact YAML mapping",
            "[uimodel][unit][presentation-preference][yaml]")
  {
    auto state = ListPresentations::Snapshot{};
    state[ListId{10}] = "plugin-preset";
    auto tree = ryml::Tree{yaml::callbacks()};

    REQUIRE(ListPresentationPreferenceYamlSchema{kLibraryId}.serialize(tree.rootref(), state));
    CHECK(tree.rootref().num_children() == 3);
    CHECK(yaml::scalarView(tree.rootref()["version"]) == "1");
    CHECK(yaml::scalarView(tree.rootref()["libraryId"]) == "123e4567-e89b-12d3-a456-426614174000");
    REQUIRE(tree.rootref()["preferences"].is_seq());
    REQUIRE(tree.rootref()["preferences"].num_children() == 1);
    CHECK(yaml::scalarView(tree.rootref()["preferences"][0]["presentationId"]) == "plugin-preset");

    auto const decodedRes =
      ListPresentationPreferenceYamlSchema{kLibraryId}.deserialize(tree.rootref(), ListPresentations::Snapshot{});
    REQUIRE(decodedRes);
    CHECK((*decodedRes) == state);
  }

  TEST_CASE(
    "ListPresentationPreferenceYamlSchema - a document saved for another library keeps only virtual-list preferences",
    "[uimodel][unit][presentation-preference][yaml]")
  {
    auto state = ListPresentations::Snapshot{{ListId{10}, "albums"}, {rt::kAllTracksListId, "songs"}};
    auto const otherLibraryId = utility::parseUuid("00000000-0000-4000-8000-000000000001").value();
    auto tree = ryml::Tree{yaml::callbacks()};
    REQUIRE(ListPresentationPreferenceYamlSchema{otherLibraryId}.serialize(tree.rootref(), state));

    auto const decodedRes = ListPresentationPreferenceYamlSchema{kLibraryId}.deserialize(tree.rootref(), {});

    REQUIRE(decodedRes);
    // List 10 names an unrelated list here; All Tracks is the same view in every library.
    CHECK((*decodedRes) == ListPresentations::Snapshot{{rt::kAllTracksListId, "songs"}});
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
        ListPresentationPreferenceYamlSchema{kLibraryId}.deserialize(tree.rootref(), ListPresentations::Snapshot{});

      REQUIRE_FALSE(decodedRes);
      CHECK(decodedRes.error().code == Error::Code::NotSupported);
    }

    SECTION("Missing required fields are rejected")
    {
      auto const* source = "version: 1\nlibraryId: 123e4567-e89b-12d3-a456-426614174000\n";
      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(source), &tree);
      auto const decodedRes =
        ListPresentationPreferenceYamlSchema{kLibraryId}.deserialize(tree.rootref(), ListPresentations::Snapshot{});

      REQUIRE_FALSE(decodedRes);
      CHECK(decodedRes.error().code == Error::Code::FormatRejected);
      CHECK(decodedRes.error().message.contains("preferences"));
    }

    SECTION("Missing library id is rejected")
    {
      auto const* source = "version: 1\npreferences: []\n";
      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(source), &tree);
      auto const decodedRes =
        ListPresentationPreferenceYamlSchema{kLibraryId}.deserialize(tree.rootref(), ListPresentations::Snapshot{});

      REQUIRE_FALSE(decodedRes);
      CHECK(decodedRes.error().code == Error::Code::FormatRejected);
      CHECK(decodedRes.error().message.contains("libraryId"));
    }

    SECTION("Malformed library id is rejected")
    {
      auto const* source = "version: 1\nlibraryId: 123e4567\npreferences: []\n";
      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(source), &tree);
      auto const decodedRes =
        ListPresentationPreferenceYamlSchema{kLibraryId}.deserialize(tree.rootref(), ListPresentations::Snapshot{});

      REQUIRE_FALSE(decodedRes);
      CHECK(decodedRes.error().code == Error::Code::FormatRejected);
      CHECK(decodedRes.error().message.contains("libraryId"));
    }

    SECTION("Unknown structural keys are rejected")
    {
      auto const* source =
        "version: 1\nlibraryId: 123e4567-e89b-12d3-a456-426614174000\npreferences: []\nfuture: true\n";
      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(source), &tree);
      auto const decodedRes =
        ListPresentationPreferenceYamlSchema{kLibraryId}.deserialize(tree.rootref(), ListPresentations::Snapshot{});

      REQUIRE_FALSE(decodedRes);
      CHECK(decodedRes.error().code == Error::Code::FormatRejected);
      CHECK(decodedRes.error().message.contains("future"));
    }

    SECTION("Unknown preference keys are rejected")
    {
      auto const* source = R"(
        version: 1
        libraryId: 123e4567-e89b-12d3-a456-426614174000
        preferences:
          - listId: 1
            presentationId: albums
            future: true
      )";
      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(source), &tree);
      auto const decodedRes =
        ListPresentationPreferenceYamlSchema{kLibraryId}.deserialize(tree.rootref(), ListPresentations::Snapshot{});

      REQUIRE_FALSE(decodedRes);
      CHECK(decodedRes.error().code == Error::Code::FormatRejected);
      CHECK(decodedRes.error().message.contains("future"));
    }

    SECTION("Preference sequence entries must be mappings")
    {
      auto const* source = "version: 1\nlibraryId: 123e4567-e89b-12d3-a456-426614174000\npreferences:\n  - malformed\n";
      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(source), &tree);
      auto const decodedRes =
        ListPresentationPreferenceYamlSchema{kLibraryId}.deserialize(tree.rootref(), ListPresentations::Snapshot{});

      REQUIRE_FALSE(decodedRes);
      CHECK(decodedRes.error().code == Error::Code::FormatRejected);
      CHECK(decodedRes.error().message.contains("preferences"));
    }

    SECTION("Missing nested presentation id is rejected")
    {
      auto const* source = "version: 1\nlibraryId: 123e4567-e89b-12d3-a456-426614174000\npreferences:\n  - listId: 1\n";
      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(source), &tree);
      auto const decodedRes =
        ListPresentationPreferenceYamlSchema{kLibraryId}.deserialize(tree.rootref(), ListPresentations::Snapshot{});

      REQUIRE_FALSE(decodedRes);
      CHECK(decodedRes.error().code == Error::Code::FormatRejected);
      CHECK(decodedRes.error().message.contains("presentationId"));
    }

    SECTION("Malformed nested entries reject the whole candidate")
    {
      auto const* source = R"(
        version: 1
        libraryId: 123e4567-e89b-12d3-a456-426614174000
        preferences:
          - listId: malformed
            presentationId: albums
      )";
      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(source), &tree);
      auto const decodedRes =
        ListPresentationPreferenceYamlSchema{kLibraryId}.deserialize(tree.rootref(), ListPresentations::Snapshot{});

      REQUIRE_FALSE(decodedRes);
      CHECK(decodedRes.error().code == Error::Code::FormatRejected);
      CHECK(decodedRes.error().message.contains("listId"));
    }
  }
} // namespace ao::uimodel::test
