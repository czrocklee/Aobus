// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/rt/TrackField.h>
#include <ao/uimodel/library/presentation/TrackColumnLayoutStore.h>
#include <ao/uimodel/library/presentation/TrackColumnLayoutYamlSchema.h>
#include <ao/yaml/RymlAdapter.h>

#include <catch2/catch_test_macros.hpp>

#include <string_view>

namespace ao::uimodel::test
{
  TEST_CASE("TrackColumnLayoutYamlSchema - round-trip uses stable field ids", "[uimodel][unit][track-column-layout]")
  {
    auto state = TrackColumnLayoutState{};
    state.listLayouts[ListId{10}] = {
      TrackColumnState{.field = rt::TrackField::Artist, .weight = 1.75},
      TrackColumnState{.field = rt::TrackField::Duration, .width = 200},
    };

    auto const document = toTrackColumnLayoutDocument(state);

    REQUIRE(document);
    CHECK(document->version == 1);
    REQUIRE(document->layouts.size() == 1);
    CHECK(document->layouts[0].listId == 10);
    REQUIRE(document->layouts[0].columns.size() == 2);
    CHECK(document->layouts[0].columns[0].field == "artist");
    CHECK(document->layouts[0].columns[1].field == "duration");

    auto const decoded = trackColumnLayoutStateFromDocument(*document);

    REQUIRE(decoded);
    REQUIRE(decoded->listLayouts.size() == 1);
    REQUIRE(decoded->listLayouts.at(ListId{10}).size() == 2);
    CHECK(decoded->listLayouts.at(ListId{10})[0] == state.listLayouts.at(ListId{10})[0]);
    CHECK(decoded->listLayouts.at(ListId{10})[1] == state.listLayouts.at(ListId{10})[1]);
  }

  TEST_CASE("TrackColumnLayoutYamlSchema - rejects an invalid document as one object",
            "[uimodel][unit][track-column-layout]")
  {
    auto document = TrackColumnLayoutDocument{
      .layouts =
        {
          StoredTrackColumnLayout{
            .listId = 10,
            .columns = {StoredTrackColumn{.field = "title", .weight = 1.0}},
          },
        },
    };

    SECTION("Unsupported version")
    {
      document.version = 2;
      auto const result = trackColumnLayoutStateFromDocument(document);

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::NotSupported);
    }

    SECTION("Unknown field")
    {
      document.layouts[0].columns[0].field = "future-field";
      auto const result = trackColumnLayoutStateFromDocument(document);

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::FormatRejected);
    }

    SECTION("Duplicate field")
    {
      document.layouts[0].columns.push_back(document.layouts[0].columns[0]);
      auto const result = trackColumnLayoutStateFromDocument(document);

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::FormatRejected);
    }

    SECTION("Invalid width and weight")
    {
      document.layouts[0].columns[0].width = 200;
      auto const result = trackColumnLayoutStateFromDocument(document);

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::FormatRejected);
    }

    SECTION("Flexible field uses fixed form")
    {
      document.layouts[0].columns[0] = StoredTrackColumn{.field = "title", .width = 200};
      auto const result = trackColumnLayoutStateFromDocument(document);

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::FormatRejected);
    }

    SECTION("Fixed field uses flexible form")
    {
      document.layouts[0].columns[0] = StoredTrackColumn{.field = "duration", .weight = 1.0};
      auto const result = trackColumnLayoutStateFromDocument(document);

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::FormatRejected);
    }

    SECTION("Invalid list id")
    {
      document.layouts[0].listId = kInvalidListId.raw();
      auto const result = trackColumnLayoutStateFromDocument(document);

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::FormatRejected);
    }
  }

  TEST_CASE("TrackColumnLayoutYamlSchema - refuses to serialize noncanonical live state",
            "[uimodel][unit][track-column-layout]")
  {
    auto state = TrackColumnLayoutState{};

    SECTION("Column has neither a width nor a weight")
    {
      state.listLayouts[ListId{10}] = {
        TrackColumnState{.field = rt::TrackField::Title},
      };
    }

    SECTION("Flexible field uses fixed form")
    {
      state.listLayouts[ListId{10}] = {
        TrackColumnState{.field = rt::TrackField::Title, .width = 200},
      };
    }

    SECTION("Fixed field uses flexible form")
    {
      state.listLayouts[ListId{10}] = {
        TrackColumnState{.field = rt::TrackField::Duration, .weight = 1.0},
      };
    }

    auto const result = toTrackColumnLayoutDocument(state);

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::InvalidState);
  }

  TEST_CASE("TrackColumnLayoutYamlSchema - owns the exact YAML mapping", "[uimodel][unit][track-column-layout][yaml]")
  {
    auto state = TrackColumnLayoutState{};
    state.listLayouts[ListId{10}] = {
      TrackColumnState{.field = rt::TrackField::Artist, .weight = 1.75},
    };
    auto tree = ryml::Tree{yaml::callbacks()};

    REQUIRE(TrackColumnLayoutYamlSchema{}.serialize(tree.rootref(), state));
    CHECK(yaml::scalarView(tree.rootref()["version"]) == "1");
    REQUIRE(tree.rootref()["layouts"].is_seq());
    CHECK(yaml::scalarView(tree.rootref()["layouts"][0]["columns"][0]["field"]) == "artist");

    auto const decoded = TrackColumnLayoutYamlSchema{}.deserialize(tree.rootref(), TrackColumnLayoutState{});
    REQUIRE(decoded);
    CHECK(decoded->listLayouts == state.listLayouts);
  }

  TEST_CASE("TrackColumnLayoutYamlSchema - rejects invalid YAML candidates",
            "[uimodel][unit][track-column-layout][yaml]")
  {
    SECTION("Future version is reported before interpreting its payload")
    {
      auto const* source = "version: 99\nlayouts: malformed\nfuture: true\n";
      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(source), &tree);
      auto const decoded = TrackColumnLayoutYamlSchema{}.deserialize(tree.rootref(), TrackColumnLayoutState{});

      REQUIRE_FALSE(decoded);
      CHECK(decoded.error().code == Error::Code::NotSupported);
    }

    SECTION("Missing required fields are rejected")
    {
      auto const* source = "version: 1\n";
      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(source), &tree);
      auto const decoded = TrackColumnLayoutYamlSchema{}.deserialize(tree.rootref(), TrackColumnLayoutState{});

      REQUIRE_FALSE(decoded);
      CHECK(decoded.error().code == Error::Code::FormatRejected);
      CHECK(decoded.error().message.contains("layouts"));
    }

    SECTION("Unknown structural keys are rejected")
    {
      auto const* source = "version: 1\nlayouts: []\nfuture: true\n";
      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(source), &tree);
      auto const decoded = TrackColumnLayoutYamlSchema{}.deserialize(tree.rootref(), TrackColumnLayoutState{});

      REQUIRE_FALSE(decoded);
      CHECK(decoded.error().code == Error::Code::FormatRejected);
      CHECK(decoded.error().message.contains("future"));
    }

    SECTION("Malformed nested entries reject the whole candidate")
    {
      auto const* source = R"(
        version: 1
        layouts:
          - listId: 10
            columns:
              - field: artist
                width: -1
                weight: malformed
      )";
      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(source), &tree);
      auto const decoded = TrackColumnLayoutYamlSchema{}.deserialize(tree.rootref(), TrackColumnLayoutState{});

      REQUIRE_FALSE(decoded);
      CHECK(decoded.error().code == Error::Code::FormatRejected);
      CHECK(decoded.error().message.contains("weight"));
    }
  }
} // namespace ao::uimodel::test
