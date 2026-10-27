// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/library/presentation/TrackColumnLayoutYamlSchema.h>

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/uimodel/library/presentation/TrackColumnLayouts.h>
#include <ao/utility/Uuid.h>
#include <ao/yaml/RymlAdapter.h>

#include <catch2/catch_test_macros.hpp>

#include <limits>

namespace ao::uimodel::test
{
  namespace
  {
    auto const kLibraryId = utility::parseUuid("123e4567-e89b-12d3-a456-426614174000").value();
  } // namespace

  TEST_CASE("TrackColumnLayoutYamlSchema - round-trip uses stable field ids")
  {
    auto state = TrackColumnLayouts::Snapshot{};
    state[ListId{10}] = {
      TrackColumnState{.field = rt::TrackField::Artist, .weight = 1.75},
      TrackColumnState{.field = rt::TrackField::Duration, .width = 200, .visible = false},
    };

    auto const documentRes = toTrackColumnLayoutDocument(state, kLibraryId);

    REQUIRE(documentRes);
    CHECK(documentRes->version == 1);
    CHECK(documentRes->libraryId == kLibraryId);
    REQUIRE(documentRes->layouts.size() == 1);
    CHECK(documentRes->layouts[0].listId == 10);
    REQUIRE(documentRes->layouts[0].columns.size() == 2);
    CHECK(documentRes->layouts[0].columns[0].field == "artist");
    CHECK(documentRes->layouts[0].columns[1].field == "duration");
    CHECK_FALSE(documentRes->layouts[0].columns[1].visible);

    auto const decodedRes = trackColumnLayoutsFromDocument(*documentRes);

    REQUIRE(decodedRes);
    REQUIRE((*decodedRes).size() == 1);
    REQUIRE((*decodedRes).at(ListId{10}).size() == 2);
    CHECK((*decodedRes).at(ListId{10})[0] == state.at(ListId{10})[0]);
    CHECK((*decodedRes).at(ListId{10})[1] == state.at(ListId{10})[1]);
  }

  TEST_CASE("TrackColumnLayoutYamlSchema - rejects an invalid document as one object")
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
      auto const res = trackColumnLayoutsFromDocument(document);

      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::NotSupported);
    }

    SECTION("Unknown field")
    {
      document.layouts[0].columns[0].field = "future-field";
      auto const res = trackColumnLayoutsFromDocument(document);

      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::FormatRejected);
    }

    SECTION("Duplicate field")
    {
      document.layouts[0].columns.push_back(document.layouts[0].columns[0]);
      auto const res = trackColumnLayoutsFromDocument(document);

      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::FormatRejected);
    }

    SECTION("Duplicate list id")
    {
      document.layouts.push_back(document.layouts[0]);
      auto const res = trackColumnLayoutsFromDocument(document);

      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::FormatRejected);
    }

    SECTION("Invalid width and weight")
    {
      document.layouts[0].columns[0].width = 200;
      auto const res = trackColumnLayoutsFromDocument(document);

      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::FormatRejected);
    }

    SECTION("Flexible weight is zero")
    {
      document.layouts[0].columns[0].weight = 0.0;
      auto const res = trackColumnLayoutsFromDocument(document);

      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::FormatRejected);
    }

    SECTION("Flexible weight is negative")
    {
      document.layouts[0].columns[0].weight = -2.0;
      auto const res = trackColumnLayoutsFromDocument(document);

      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::FormatRejected);
    }

    SECTION("Flexible weight is positive infinity")
    {
      document.layouts[0].columns[0].weight = std::numeric_limits<double>::infinity();
      auto const res = trackColumnLayoutsFromDocument(document);

      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::FormatRejected);
    }

    SECTION("Flexible weight is NaN")
    {
      document.layouts[0].columns[0].weight = std::numeric_limits<double>::quiet_NaN();
      auto const res = trackColumnLayoutsFromDocument(document);

      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::FormatRejected);
    }

    SECTION("Fixed width is zero")
    {
      document.layouts[0].columns[0] = StoredTrackColumn{.field = "duration", .width = 0};
      auto const res = trackColumnLayoutsFromDocument(document);

      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::FormatRejected);
    }

    SECTION("Fixed width is negative")
    {
      document.layouts[0].columns[0] = StoredTrackColumn{.field = "duration", .width = -2};
      auto const res = trackColumnLayoutsFromDocument(document);

      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::FormatRejected);
    }

    SECTION("Flexible field uses fixed form")
    {
      document.layouts[0].columns[0] = StoredTrackColumn{.field = "title", .width = 200};
      auto const res = trackColumnLayoutsFromDocument(document);

      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::FormatRejected);
    }

    SECTION("Fixed field uses flexible form")
    {
      document.layouts[0].columns[0] = StoredTrackColumn{.field = "duration", .weight = 1.0};
      auto const res = trackColumnLayoutsFromDocument(document);

      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::FormatRejected);
    }

    SECTION("Invalid list id")
    {
      document.layouts[0].listId = kInvalidListId.raw();
      auto const res = trackColumnLayoutsFromDocument(document);

      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::FormatRejected);
    }
  }

  TEST_CASE("TrackColumnLayoutYamlSchema - owns the exact YAML mapping")
  {
    auto state = TrackColumnLayouts::Snapshot{};
    state[ListId{10}] = {
      TrackColumnState{.field = rt::TrackField::Artist, .weight = 1.75},
      TrackColumnState{.field = rt::TrackField::Duration, .width = 200, .visible = false},
    };
    auto tree = ryml::Tree{yaml::callbacks()};

    REQUIRE(TrackColumnLayoutYamlSchema{kLibraryId}.serialize(tree.rootref(), state));
    auto const root = tree.rootref();
    CHECK(root.num_children() == 3);
    CHECK(yaml::scalarView(root["version"]) == "1");
    CHECK(yaml::scalarView(root["libraryId"]) == "123e4567-e89b-12d3-a456-426614174000");
    REQUIRE(root["layouts"].is_seq());
    REQUIRE(root["layouts"].num_children() == 1);
    auto const layout = root["layouts"][0];
    CHECK(layout.num_children() == 2);
    CHECK(yaml::scalarView(layout["listId"]) == "10");
    REQUIRE(layout["columns"].is_seq());
    REQUIRE(layout["columns"].num_children() == 2);
    auto const flexibleColumn = layout["columns"][0];
    CHECK(flexibleColumn.num_children() == 4);
    CHECK(yaml::scalarView(flexibleColumn["field"]) == "artist");
    CHECK(yaml::scalarView(flexibleColumn["width"]) == "-1");
    CHECK(yaml::scalarView(flexibleColumn["weight"]) == "1.75");
    CHECK(yaml::scalarView(flexibleColumn["visible"]) == "true");
    auto const fixedColumn = layout["columns"][1];
    CHECK(fixedColumn.num_children() == 4);
    CHECK(yaml::scalarView(fixedColumn["field"]) == "duration");
    CHECK(yaml::scalarView(fixedColumn["width"]) == "200");
    CHECK(yaml::scalarView(fixedColumn["weight"]) == "-1");
    CHECK(yaml::scalarView(fixedColumn["visible"]) == "false");

    auto const decodedRes =
      TrackColumnLayoutYamlSchema{kLibraryId}.deserialize(tree.rootref(), TrackColumnLayouts::Snapshot{});
    REQUIRE(decodedRes);
    CHECK((*decodedRes) == state);
  }

  TEST_CASE("TrackColumnLayoutYamlSchema - a document saved for another library keeps only virtual-list layouts")
  {
    auto state = TrackColumnLayouts::Snapshot{};
    state[ListId{10}] = {TrackColumnState{.field = rt::TrackField::Duration, .width = 200}};
    state[rt::kAllTracksListId] = {TrackColumnState{.field = rt::TrackField::Artist, .weight = 1.5}};
    auto const otherLibraryId = utility::parseUuid("00000000-0000-4000-8000-000000000001").value();
    auto tree = ryml::Tree{yaml::callbacks()};
    REQUIRE(TrackColumnLayoutYamlSchema{otherLibraryId}.serialize(tree.rootref(), state));

    auto const decodedRes = TrackColumnLayoutYamlSchema{kLibraryId}.deserialize(tree.rootref(), {});

    REQUIRE(decodedRes);
    // List 10 names an unrelated list here; All Tracks is the same view in every library.
    CHECK((*decodedRes) == TrackColumnLayouts::Snapshot{{rt::kAllTracksListId, state.at(rt::kAllTracksListId)}});
  }

  TEST_CASE("TrackColumnLayoutYamlSchema - rejects invalid YAML candidates")
  {
    SECTION("Future version is reported before interpreting its payload")
    {
      auto const* source = "version: 99\nlayouts: malformed\nfuture: true\n";
      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(source), &tree);
      auto const decodedRes =
        TrackColumnLayoutYamlSchema{kLibraryId}.deserialize(tree.rootref(), TrackColumnLayouts::Snapshot{});

      REQUIRE_FALSE(decodedRes);
      CHECK(decodedRes.error().code == Error::Code::NotSupported);
    }

    SECTION("Missing required fields are rejected")
    {
      auto const* source = "version: 1\nlibraryId: 123e4567-e89b-12d3-a456-426614174000\n";
      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(source), &tree);
      auto const decodedRes =
        TrackColumnLayoutYamlSchema{kLibraryId}.deserialize(tree.rootref(), TrackColumnLayouts::Snapshot{});

      REQUIRE_FALSE(decodedRes);
      CHECK(decodedRes.error().code == Error::Code::FormatRejected);
      CHECK(decodedRes.error().message.contains("layouts"));
    }

    SECTION("Missing library id is rejected")
    {
      auto const* source = "version: 1\nlayouts: []\n";
      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(source), &tree);
      auto const decodedRes =
        TrackColumnLayoutYamlSchema{kLibraryId}.deserialize(tree.rootref(), TrackColumnLayouts::Snapshot{});

      REQUIRE_FALSE(decodedRes);
      CHECK(decodedRes.error().code == Error::Code::FormatRejected);
      CHECK(decodedRes.error().message.contains("libraryId"));
    }

    SECTION("Malformed library id is rejected")
    {
      auto const* source = "version: 1\nlibraryId: 123e4567\nlayouts: []\n";
      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(source), &tree);
      auto const decodedRes =
        TrackColumnLayoutYamlSchema{kLibraryId}.deserialize(tree.rootref(), TrackColumnLayouts::Snapshot{});

      REQUIRE_FALSE(decodedRes);
      CHECK(decodedRes.error().code == Error::Code::FormatRejected);
      CHECK(decodedRes.error().message.contains("libraryId"));
    }

    SECTION("Unknown structural keys are rejected")
    {
      auto const* source = "version: 1\nlibraryId: 123e4567-e89b-12d3-a456-426614174000\nlayouts: []\nfuture: true\n";
      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(source), &tree);
      auto const decodedRes =
        TrackColumnLayoutYamlSchema{kLibraryId}.deserialize(tree.rootref(), TrackColumnLayouts::Snapshot{});

      REQUIRE_FALSE(decodedRes);
      CHECK(decodedRes.error().code == Error::Code::FormatRejected);
      CHECK(decodedRes.error().message.contains("future"));
    }

    SECTION("Unknown layout keys are rejected")
    {
      auto const* source = R"(
        version: 1
        libraryId: 123e4567-e89b-12d3-a456-426614174000
        layouts:
          - listId: 10
            columns: []
            future: true
      )";
      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(source), &tree);
      auto const decodedRes =
        TrackColumnLayoutYamlSchema{kLibraryId}.deserialize(tree.rootref(), TrackColumnLayouts::Snapshot{});

      REQUIRE_FALSE(decodedRes);
      CHECK(decodedRes.error().code == Error::Code::FormatRejected);
      CHECK(decodedRes.error().message.contains("future"));
    }

    SECTION("Layout sequence entries must be mappings")
    {
      auto const* source = "version: 1\nlibraryId: 123e4567-e89b-12d3-a456-426614174000\nlayouts:\n  - malformed\n";
      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(source), &tree);
      auto const decodedRes =
        TrackColumnLayoutYamlSchema{kLibraryId}.deserialize(tree.rootref(), TrackColumnLayouts::Snapshot{});

      REQUIRE_FALSE(decodedRes);
      CHECK(decodedRes.error().code == Error::Code::FormatRejected);
      CHECK(decodedRes.error().message.contains("layouts"));
    }

    SECTION("Layout columns are required")
    {
      auto const* source = "version: 1\nlibraryId: 123e4567-e89b-12d3-a456-426614174000\nlayouts:\n  - listId: 10\n";
      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(source), &tree);
      auto const decodedRes =
        TrackColumnLayoutYamlSchema{kLibraryId}.deserialize(tree.rootref(), TrackColumnLayouts::Snapshot{});

      REQUIRE_FALSE(decodedRes);
      CHECK(decodedRes.error().code == Error::Code::FormatRejected);
      CHECK(decodedRes.error().message.contains("columns"));
    }

    SECTION("Unknown column keys are rejected")
    {
      auto const* source = R"(
        version: 1
        libraryId: 123e4567-e89b-12d3-a456-426614174000
        layouts:
          - listId: 10
            columns:
              - field: artist
                width: -1
                weight: 1
                visible: true
                future: true
      )";
      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(source), &tree);
      auto const decodedRes =
        TrackColumnLayoutYamlSchema{kLibraryId}.deserialize(tree.rootref(), TrackColumnLayouts::Snapshot{});

      REQUIRE_FALSE(decodedRes);
      CHECK(decodedRes.error().code == Error::Code::FormatRejected);
      CHECK(decodedRes.error().message.contains("future"));
    }

    SECTION("Column sequence entries must be mappings")
    {
      auto const* source = "version: 1\nlibraryId: 123e4567-e89b-12d3-a456-426614174000\nlayouts:\n  - listId: 10\n    "
                           "columns:\n      - malformed\n";
      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(source), &tree);
      auto const decodedRes =
        TrackColumnLayoutYamlSchema{kLibraryId}.deserialize(tree.rootref(), TrackColumnLayouts::Snapshot{});

      REQUIRE_FALSE(decodedRes);
      CHECK(decodedRes.error().code == Error::Code::FormatRejected);
      CHECK(decodedRes.error().message.contains("columns"));
    }

    SECTION("Malformed nested entries reject the whole candidate")
    {
      auto const* source = R"(
        version: 1
        libraryId: 123e4567-e89b-12d3-a456-426614174000
        layouts:
          - listId: 10
            columns:
              - field: artist
                width: -1
                weight: malformed
                visible: true
      )";
      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(source), &tree);
      auto const decodedRes =
        TrackColumnLayoutYamlSchema{kLibraryId}.deserialize(tree.rootref(), TrackColumnLayouts::Snapshot{});

      REQUIRE_FALSE(decodedRes);
      CHECK(decodedRes.error().code == Error::Code::FormatRejected);
      CHECK(decodedRes.error().message.contains("weight"));
    }

    SECTION("Column visibility is required")
    {
      auto const* source = R"(
        version: 1
        libraryId: 123e4567-e89b-12d3-a456-426614174000
        layouts:
          - listId: 10
            columns:
              - field: artist
                width: -1
                weight: 1
      )";
      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(source), &tree);
      auto const decodedRes =
        TrackColumnLayoutYamlSchema{kLibraryId}.deserialize(tree.rootref(), TrackColumnLayouts::Snapshot{});

      REQUIRE_FALSE(decodedRes);
      CHECK(decodedRes.error().code == Error::Code::FormatRejected);
      CHECK(decodedRes.error().message.contains("visible"));
    }
  }
} // namespace ao::uimodel::test
