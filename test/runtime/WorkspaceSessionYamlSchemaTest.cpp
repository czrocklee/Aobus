// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "runtime/WorkspaceSessionYamlSchema.h"

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/WorkspaceSessionState.h>
#include <ao/utility/Uuid.h>
#include <ao/yaml/RymlAdapter.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    TrackPresentationSpec makePresentation()
    {
      return TrackPresentationSpec{
        .presetId = CustomPresetId{0xfedcba9876543210ULL},
        .groupBy = TrackGroupKey::Album,
        .sortBy =
          {
            TrackSortTerm{.field = TrackSortField::DiscNumber, .ascending = true},
            TrackSortTerm{.field = TrackSortField::Title, .ascending = false},
          },
        .visibleFields = {TrackField::Title, TrackField::Duration},
        .redundantFields = {TrackField::Album},
      };
    }

    Result<WorkspaceSessionState> decodeWorkspaceSessionYaml(char const* source)
    {
      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(source), &tree);
      return detail::WorkspaceSessionYamlSchema{}.deserialize(tree.rootref(), WorkspaceSessionState{});
    }
  } // namespace

  TEST_CASE("WorkspaceSessionYamlSchema - round-trip uses stable presentation vocabulary")
  {
    auto const presentation = makePresentation();
    auto secondPresentation = presentation;
    secondPresentation.presetId = CustomPresetId{42};
    secondPresentation.optColumnSort = TrackColumnSort{.column = TrackField::Title, .ascending = true};
    secondPresentation.groupBy = TrackGroupKey::Artist;
    secondPresentation.sortBy = {
      {.field = TrackSortField::Artist, .ascending = true}, {.field = TrackSortField::Title, .ascending = false}};
    secondPresentation.visibleFields = {TrackField::Artist, TrackField::Title};
    secondPresentation.redundantFields = {TrackField::AlbumArtist};
    auto const state = WorkspaceSessionState{
      .openViews =
        {
          TrackListViewConfig{
            .listId = ListId{10},
            .filterExpression = "$genre = \"Jazz\"",
            .optPresentation = presentation,
          },
          TrackListViewConfig{
            .listId = ListId{11},
            .filterExpression = "$genre = \"Classical\"",
            .optPresentation = secondPresentation,
          },
        },
      .activeViewIndex = 1,
      .customPresets =
        {
          CustomTrackPresentationPreset{
            .label = "Descending Albums",
            .optBasePresetId = BuiltinPreset::Albums,
            .spec = presentation,
          },
        },
    };

    auto const documentRes = detail::toWorkspaceSessionDocument(state);

    REQUIRE(documentRes);
    CHECK(documentRes->presentationVersion == 3);
    REQUIRE(documentRes->openViews.size() == 2);
    CHECK(documentRes->activeViewIndex == 1);
    auto const& firstStoredView = documentRes->openViews[0];
    CHECK(firstStoredView.listId == 10);
    CHECK(firstStoredView.filterExpression == "$genre = \"Jazz\"");
    auto const& firstStored = firstStoredView.presentation;
    CHECK(firstStored.presetId == presentation.presetId);
    CHECK(firstStored.group == "album");
    REQUIRE(firstStored.sort.size() == 2);
    CHECK(firstStored.sort[0].field == "disc-number");
    CHECK(firstStored.sort[0].direction == "ascending");
    CHECK(firstStored.sort[1].field == "title");
    CHECK(firstStored.sort[1].direction == "descending");
    CHECK(firstStored.visibleFields == std::vector<std::string>{"title", "duration"});
    CHECK(firstStored.redundantFields == std::vector<std::string>{"album"});
    auto const& secondStoredView = documentRes->openViews[1];
    CHECK(secondStoredView.listId == 11);
    CHECK(secondStoredView.filterExpression == "$genre = \"Classical\"");
    auto const& secondStored = secondStoredView.presentation;
    CHECK(secondStored.presetId == TrackPresetId{CustomPresetId{42}});
    REQUIRE(secondStored.optColumnSort);
    CHECK(secondStored.optColumnSort->column == "title");
    CHECK(secondStored.optColumnSort->direction == "ascending");
    CHECK(secondStored.group == "artist");
    REQUIRE(secondStored.sort.size() == 2);
    CHECK(secondStored.sort[0].field == "artist");
    CHECK(secondStored.sort[0].direction == "ascending");
    CHECK(secondStored.sort[1].field == "title");
    CHECK(secondStored.sort[1].direction == "descending");
    CHECK(secondStored.visibleFields == std::vector<std::string>{"artist", "title"});
    CHECK(secondStored.redundantFields == std::vector<std::string>{"album-artist"});

    auto const decodedRes = detail::workspaceSessionStateFromDocument(*documentRes);

    REQUIRE(decodedRes);
    REQUIRE(decodedRes->openViews.size() == 2);
    REQUIRE(decodedRes->openViews[0].optPresentation);
    CHECK(decodedRes->openViews[0].listId == ListId{10});
    CHECK(decodedRes->openViews[0].filterExpression == "$genre = \"Jazz\"");
    CHECK(*decodedRes->openViews[0].optPresentation == presentation);
    REQUIRE(decodedRes->openViews[1].optPresentation);
    CHECK(decodedRes->openViews[1].listId == ListId{11});
    CHECK(decodedRes->openViews[1].filterExpression == "$genre = \"Classical\"");
    CHECK(*decodedRes->openViews[1].optPresentation == secondPresentation);
    CHECK(decodedRes->activeViewIndex == 1);
    REQUIRE(decodedRes->customPresets.size() == 1);
    CHECK(decodedRes->customPresets[0] == state.customPresets[0]);
  }

  TEST_CASE("WorkspaceSessionYamlSchema - empty workspace uses active view index zero")
  {
    auto const documentRes = detail::toWorkspaceSessionDocument(WorkspaceSessionState{});

    REQUIRE(documentRes);
    CHECK(documentRes->openViews.empty());
    CHECK(documentRes->activeViewIndex == 0);

    auto const decodedRes = detail::workspaceSessionStateFromDocument(*documentRes);
    REQUIRE(decodedRes);
    CHECK(decodedRes->openViews.empty());
    CHECK(decodedRes->activeViewIndex == 0);
  }

  TEST_CASE("WorkspaceSessionYamlSchema - canonicalizes permitted live presentation state")
  {
    auto const duplicateFields = TrackPresentationSpec{
      .presetId = CustomPresetId{0},
      .visibleFields = {TrackField::Title, TrackField::Title},
      .redundantFields = {TrackField::Album, TrackField::Album},
    };
    auto const state = WorkspaceSessionState{
      .openViews =
        {
          TrackListViewConfig{
            .listId = ListId{10},
            .optPresentation =
              TrackPresentationSpec{.presetId = BuiltinPreset::ListOrder,
                                    .optColumnSort = TrackColumnSort{.column = TrackField::Title, .ascending = false}},
          },
          TrackListViewConfig{.listId = ListId{11}, .optPresentation = duplicateFields},
        },
    };

    auto const documentRes = detail::toWorkspaceSessionDocument(state);

    REQUIRE(documentRes);
    REQUIRE(documentRes->openViews.size() == 2);
    CHECK(documentRes->openViews[0].presentation.visibleFields == std::vector<std::string>{"title"});
    CHECK(documentRes->openViews[1].presentation.visibleFields == std::vector<std::string>{"title"});
    CHECK(documentRes->openViews[1].presentation.redundantFields == std::vector<std::string>{"album"});

    auto const decodedRes = detail::workspaceSessionStateFromDocument(*documentRes);
    REQUIRE(decodedRes);
    REQUIRE(decodedRes->openViews.size() == 2);
    REQUIRE(decodedRes->openViews[0].optPresentation);
    CHECK(decodedRes->openViews[0].optPresentation->presetId == TrackPresetId{BuiltinPreset::ListOrder});
    CHECK(decodedRes->openViews[0].optPresentation->sortBy.empty());
    CHECK(decodedRes->openViews[0].optPresentation->optColumnSort == TrackColumnSort{TrackField::Title, false});
  }

  TEST_CASE("WorkspaceSessionYamlSchema - rejects invalid persisted state")
  {
    auto const validState = WorkspaceSessionState{
      .openViews =
        {
          TrackListViewConfig{.listId = ListId{10}, .optPresentation = makePresentation()},
        },
    };
    auto documentRes = detail::toWorkspaceSessionDocument(validState);
    REQUIRE(documentRes);
    auto document = std::move(*documentRes);
    auto& presentation = document.openViews[0].presentation;
    auto expectedCode = Error::Code::FormatRejected;

    SECTION("Unsupported version")
    {
      document.presentationVersion = 2;
      expectedCode = Error::Code::NotSupported;
    }

    SECTION("Invalid list id")
    {
      document.openViews[0].listId = kInvalidListId.raw();
    }

    SECTION("Nonempty workspace active index is out of bounds")
    {
      document.activeViewIndex = 1;
    }

    SECTION("Empty workspace active index is nonzero")
    {
      document.openViews.clear();
      document.activeViewIndex = 1;
    }

    SECTION("Unknown column")
    {
      presentation.optColumnSort = detail::StoredTrackColumnSort{.column = "future-column", .direction = "descending"};
    }

    SECTION("Known but unsortable column")
    {
      presentation.optColumnSort = detail::StoredTrackColumnSort{.column = "movement-total", .direction = "ascending"};
    }

    SECTION("Unknown column sort direction")
    {
      presentation.optColumnSort = detail::StoredTrackColumnSort{.column = "title", .direction = "sideways"};
    }

    SECTION("Unknown group")
    {
      presentation.group = "future-group";
    }

    SECTION("Unknown sort field")
    {
      presentation.sort[0].field = "future-sort";
    }

    SECTION("Unknown sort direction")
    {
      presentation.sort[0].direction = "sideways";
    }

    SECTION("Duplicate sort field")
    {
      presentation.sort.push_back(presentation.sort[0]);
    }

    SECTION("Unknown visible field")
    {
      presentation.visibleFields[0] = "future-field";
    }

    SECTION("No visible fields")
    {
      presentation.visibleFields.clear();
    }

    SECTION("Duplicate visible field")
    {
      presentation.visibleFields.push_back(presentation.visibleFields[0]);
    }

    SECTION("Unknown redundant field")
    {
      presentation.redundantFields[0] = "future-field";
    }

    SECTION("Duplicate redundant field")
    {
      presentation.redundantFields.push_back(presentation.redundantFields[0]);
    }

    SECTION("Invalid custom preset")
    {
      auto& preset = document.customPresets.emplace_back();
      preset.spec = presentation;

      SECTION("Custom definitions cannot contain a column sort override")
      {
        preset.spec.optColumnSort = detail::StoredTrackColumnSort{.column = "title", .direction = "ascending"};
      }

      SECTION("Builtin identity")
      {
        preset.spec.presetId = BuiltinPreset::Albums;
      }

      SECTION("Unknown base key")
      {
        preset.basePresetId = "unknown";
      }
    }

    auto const res = detail::workspaceSessionStateFromDocument(document);

    REQUIRE_FALSE(res);
    CHECK(res.error().code == expectedCode);
  }

  TEST_CASE("WorkspaceSessionYamlSchema - owns the exact YAML mapping")
  {
    auto const presetSpec = makePresentation();
    auto presentation = presetSpec;
    auto expectedColumn = std::string_view{};
    auto expectedDirection = std::string_view{};
    auto siblingColumn = TrackField{};

    SECTION("Display track number keeps its column identity")
    {
      presentation.optColumnSort = TrackColumnSort{.column = TrackField::DisplayTrackNumber, .ascending = false};
      siblingColumn = TrackField::TrackNumber;
      expectedColumn = "display-track-number";
      expectedDirection = "descending";
    }

    SECTION("Movement number keeps its column identity")
    {
      presentation.optColumnSort = TrackColumnSort{.column = TrackField::MovementNumber, .ascending = true};
      siblingColumn = TrackField::Movement;
      expectedColumn = "movement-number";
      expectedDirection = "ascending";
    }

    auto const state = WorkspaceSessionState{
      .libraryId = utility::parseUuid("123e4567-e89b-12d3-a456-426614174000").value(),
      .openViews =
        {
          TrackListViewConfig{
            .listId = ListId{10},
            .filterExpression = "$genre = \"Jazz\"",
            .optPresentation = presentation,
          },
          TrackListViewConfig{
            .listId = ListId{11}, .optPresentation = builtinTrackPresentationPreset(BuiltinPreset::Artists).spec},
        },
      .activeViewIndex = 0,
      .customPresets =
        {
          CustomTrackPresentationPreset{
            .label = "Descending Albums",
            .optBasePresetId = CustomPresetId{0xabc},
            .spec = presetSpec,
          },
        },
    };
    auto tree = ryml::Tree{yaml::callbacks()};

    REQUIRE(detail::WorkspaceSessionYamlSchema{}.serialize(tree.rootref(), state));
    auto const encoded = ryml::emitrs_yaml<std::string>(tree);
    auto const versionPosition = encoded.find("presentationVersion:");
    auto const libraryPosition = encoded.find("libraryId:");
    auto const viewsPosition = encoded.find("openViews:");
    auto const activePosition = encoded.find("activeViewIndex:");
    auto const presetsPosition = encoded.find("customPresets:");
    REQUIRE(versionPosition != std::string::npos);
    REQUIRE(libraryPosition != std::string::npos);
    REQUIRE(viewsPosition != std::string::npos);
    REQUIRE(activePosition != std::string::npos);
    REQUIRE(presetsPosition != std::string::npos);
    CHECK(tree.rootref().num_children() == 5);
    CHECK(versionPosition < libraryPosition);
    CHECK(libraryPosition < viewsPosition);
    CHECK(viewsPosition < activePosition);
    CHECK(activePosition < presetsPosition);
    auto const root = tree.rootref();
    CHECK(yaml::scalarView(root["presentationVersion"]) == "3");
    CHECK(yaml::scalarView(root["libraryId"]) == "123e4567-e89b-12d3-a456-426614174000");
    CHECK(yaml::scalarView(root["activeViewIndex"]) == "0");

    REQUIRE(root["openViews"].num_children() == 2);
    auto const view = root["openViews"][0];
    CHECK(view.num_children() == 3);
    CHECK(view.has_child("listId"));
    CHECK(view.has_child("filterExpression"));
    CHECK(view.has_child("presentation"));
    auto const checkPresentationShape = [](ryml::ConstNodeRef const presentationNode)
    {
      REQUIRE(presentationNode.has_child("kind"));
      CHECK(presentationNode.has_child("group"));
      CHECK(presentationNode.has_child("sort"));
      CHECK(presentationNode.has_child("visibleFields"));
      CHECK(presentationNode.has_child("redundantFields"));
      REQUIRE(presentationNode["sort"].num_children() == 2);

      for (auto const sortTerm : presentationNode["sort"].children())
      {
        CHECK(sortTerm.num_children() == 2);
        CHECK(sortTerm.has_child("field"));
        CHECK(sortTerm.has_child("direction"));
      }
    };
    auto const viewPresentation = view["presentation"];
    checkPresentationShape(viewPresentation);
    CHECK(viewPresentation.num_children() == 7);
    CHECK(yaml::scalarView(viewPresentation["kind"]) == "custom");
    CHECK(yaml::scalarView(viewPresentation["presetId"]) == "fedcba9876543210");
    REQUIRE(viewPresentation.has_child("columnSort"));
    auto const columnSort = viewPresentation["columnSort"];
    REQUIRE(columnSort.has_child("column"));
    CHECK(columnSort.num_children() == 2);
    CHECK(yaml::scalarView(columnSort["column"]) == expectedColumn);
    CHECK(yaml::scalarView(columnSort["direction"]) == expectedDirection);
    CHECK(yaml::scalarView(viewPresentation["sort"][0]["field"]) == "disc-number");

    auto const builtin = root["openViews"][1]["presentation"];
    CHECK_FALSE(builtin.has_child("columnSort"));
    CHECK(yaml::scalarView(builtin["kind"]) == "builtin");
    CHECK(yaml::scalarView(builtin["presetId"]) == "artists");

    REQUIRE(root["customPresets"].num_children() == 1);
    auto const preset = root["customPresets"][0];
    CHECK(preset.num_children() == 3);
    CHECK(preset.has_child("label"));
    REQUIRE(preset.has_child("basePresetId"));
    CHECK(yaml::scalarView(preset["basePresetId"]) == "0000000000000abc");
    CHECK(preset.has_child("spec"));
    auto const storedSpec = preset["spec"];
    checkPresentationShape(storedSpec);
    CHECK(storedSpec.num_children() == 6);
    CHECK_FALSE(storedSpec.has_child("columnSort"));
    CHECK(yaml::scalarView(storedSpec["kind"]) == "custom");
    REQUIRE(storedSpec.has_child("presetId"));
    CHECK(yaml::scalarView(storedSpec["presetId"]) == "fedcba9876543210");

    auto const decodedRes = detail::WorkspaceSessionYamlSchema{}.deserialize(tree.rootref(), WorkspaceSessionState{});
    REQUIRE(decodedRes);
    CHECK(decodedRes->libraryId == state.libraryId);
    REQUIRE(decodedRes->openViews.size() == 2);
    CHECK(decodedRes->openViews[1].optPresentation == state.openViews[1].optPresentation);
    CHECK(decodedRes->openViews[0].listId == ListId{10});
    REQUIRE(decodedRes->openViews[0].optPresentation);
    auto const& restored = *decodedRes->openViews[0].optPresentation;
    CHECK(restored == presentation);
    auto const clicked = *presentation.optColumnSort;
    CHECK(restored.nextColumnSort(clicked.column) ==
          TrackColumnSort{.column = clicked.column, .ascending = !clicked.ascending});
    CHECK(restored.nextColumnSort(siblingColumn) == TrackColumnSort{.column = siblingColumn, .ascending = true});
    CHECK(decodedRes->customPresets == state.customPresets);
  }

  TEST_CASE("WorkspaceSessionYamlSchema - rejects invalid YAML preset keys and column overrides")
  {
    auto const* identityFields =
      GENERATE("kind: builtin",
               "kind: builtin, presetId: unknown",
               "kind: builtin, presetId: 0000000000000001",
               "kind: custom, presetId: songs",
               "kind: custom, presetId: fff",
               "kind: custom, presetId: FEDCBA9876543210",
               "kind: columnSort",
               "kind: builtin, presetId: songs, columnSort: []",
               "kind: builtin, presetId: songs, columnSort: {field: title, direction: ascending}",
               "kind: builtin, presetId: songs, columnSort: {column: title}",
               "kind: builtin, presetId: songs, columnSort: {direction: ascending}",
               "kind: builtin, presetId: songs, columnSort: {column: year, direction: ascending, future: true}",
               "kind: unknown, presetId: songs");
    auto const source =
      std::string{R"(presentationVersion: 3
libraryId: 123e4567-e89b-12d3-a456-426614174000
openViews:
  - listId: 10
    filterExpression: ""
    presentation: {)"} +
      identityFields +
      R"(, group: none, sort: [{field: title, direction: ascending}], visibleFields: [title], redundantFields: []}
activeViewIndex: 0
customPresets: []
)";
    auto const decodedRes = decodeWorkspaceSessionYaml(source.c_str());
    REQUIRE_FALSE(decodedRes);
    CHECK(decodedRes.error().code == Error::Code::FormatRejected);
  }

  TEST_CASE("WorkspaceSessionYamlSchema - future version takes precedence over malformed payload")
  {
    auto const* source = "presentationVersion: 99\nopenViews: malformed\nactiveViewIndex: malformed\ncustomPresets: "
                         "malformed\nfuture: true\n";
    auto const decodedRes = decodeWorkspaceSessionYaml(source);

    REQUIRE_FALSE(decodedRes);
    CHECK(decodedRes.error().code == Error::Code::NotSupported);
  }

  TEST_CASE("WorkspaceSessionYamlSchema - rejects missing required YAML fields")
  {
    SECTION("Missing required fields are rejected")
    {
      auto const* source =
        "presentationVersion: 3\nlibraryId: 123e4567-e89b-12d3-a456-426614174000\nopenViews: []\nactiveViewIndex: 0\n";
      auto const decodedRes = decodeWorkspaceSessionYaml(source);

      REQUIRE_FALSE(decodedRes);
      CHECK(decodedRes.error().code == Error::Code::FormatRejected);
      CHECK(decodedRes.error().message.contains("customPresets"));
    }

    SECTION("Missing library id is rejected")
    {
      auto const* source = "presentationVersion: 3\nopenViews: []\nactiveViewIndex: 0\ncustomPresets: []\n";
      auto const decodedRes = decodeWorkspaceSessionYaml(source);

      REQUIRE_FALSE(decodedRes);
      CHECK(decodedRes.error().code == Error::Code::FormatRejected);
      CHECK(decodedRes.error().message.contains("libraryId"));
    }

    SECTION("A library id that is not a UUID is rejected")
    {
      auto const* source =
        "presentationVersion: 3\nlibraryId: 123e4567\nopenViews: []\nactiveViewIndex: 0\ncustomPresets: []\n";
      auto const decodedRes = decodeWorkspaceSessionYaml(source);

      REQUIRE_FALSE(decodedRes);
      CHECK(decodedRes.error().code == Error::Code::FormatRejected);
      CHECK(decodedRes.error().message.contains("libraryId"));
    }

    SECTION("Missing active view index is rejected")
    {
      auto const* source =
        "presentationVersion: 3\nlibraryId: 123e4567-e89b-12d3-a456-426614174000\nopenViews: []\ncustomPresets: []\n";
      auto const decodedRes = decodeWorkspaceSessionYaml(source);

      REQUIRE_FALSE(decodedRes);
      CHECK(decodedRes.error().code == Error::Code::FormatRejected);
      CHECK(decodedRes.error().message.contains("activeViewIndex"));
    }
  }

  TEST_CASE("WorkspaceSessionYamlSchema - rejects unknown YAML keys recursively")
  {
    SECTION("Unknown structural keys are rejected")
    {
      auto const* source = "presentationVersion: 3\nlibraryId: 123e4567-e89b-12d3-a456-426614174000\nopenViews: "
                           "[]\nactiveViewIndex: 0\ncustomPresets: []\nfuture: true\n";
      auto const decodedRes = decodeWorkspaceSessionYaml(source);

      REQUIRE_FALSE(decodedRes);
      CHECK(decodedRes.error().code == Error::Code::FormatRejected);
      CHECK(decodedRes.error().message.contains("future"));
    }

    SECTION("Unknown view keys are rejected")
    {
      auto const* source = R"(
        presentationVersion: 3
        libraryId: 123e4567-e89b-12d3-a456-426614174000
        openViews:
          - listId: 10
            filterExpression: ""
            presentation:
              kind: builtin
              presetId: library
              group: none
              sort: []
              visibleFields: [title]
              redundantFields: []
            future: true
        activeViewIndex: 0
        customPresets: []
      )";
      auto const decodedRes = decodeWorkspaceSessionYaml(source);

      REQUIRE_FALSE(decodedRes);
      CHECK(decodedRes.error().code == Error::Code::FormatRejected);
      CHECK(decodedRes.error().message.contains("future"));
    }

    SECTION("Unknown presentation keys are rejected")
    {
      auto const* source = R"(
        presentationVersion: 3
        libraryId: 123e4567-e89b-12d3-a456-426614174000
        openViews:
          - listId: 10
            filterExpression: ""
            presentation:
              kind: builtin
              presetId: list-order
              group: none
              sort: []
              visibleFields: [title]
              redundantFields: []
              future: true
        activeViewIndex: 0
        customPresets: []
      )";
      auto const decodedRes = decodeWorkspaceSessionYaml(source);

      REQUIRE_FALSE(decodedRes);
      CHECK(decodedRes.error().code == Error::Code::FormatRejected);
      CHECK(decodedRes.error().message.contains("future"));
    }

    SECTION("Unknown sort-term keys are rejected")
    {
      auto const* source = R"(
        presentationVersion: 3
        libraryId: 123e4567-e89b-12d3-a456-426614174000
        openViews:
          - listId: 10
            filterExpression: ""
            presentation:
              kind: builtin
              presetId: library
              group: none
              sort:
                - field: title
                  direction: ascending
                  future: true
              visibleFields: [title]
              redundantFields: []
        activeViewIndex: 0
        customPresets: []
      )";
      auto const decodedRes = decodeWorkspaceSessionYaml(source);

      REQUIRE_FALSE(decodedRes);
      CHECK(decodedRes.error().code == Error::Code::FormatRejected);
      CHECK(decodedRes.error().message.contains("future"));
    }

    SECTION("Unknown custom-preset keys are rejected")
    {
      auto const* source = R"(
        presentationVersion: 3
        libraryId: 123e4567-e89b-12d3-a456-426614174000
        openViews: []
        activeViewIndex: 0
        customPresets:
          - label: Custom
            basePresetId: library
            spec:
              kind: custom
              presetId: 0000000000000042
              group: none
              sort: []
              visibleFields: [title]
              redundantFields: []
            future: true
      )";
      auto const decodedRes = decodeWorkspaceSessionYaml(source);

      REQUIRE_FALSE(decodedRes);
      CHECK(decodedRes.error().code == Error::Code::FormatRejected);
      CHECK(decodedRes.error().message.contains("future"));
    }
  }

  TEST_CASE("WorkspaceSessionYamlSchema - rejects malformed YAML node kinds")
  {
    SECTION("Malformed view sequence elements are rejected")
    {
      auto const* source = "presentationVersion: 3\nlibraryId: 123e4567-e89b-12d3-a456-426614174000\nopenViews: "
                           "[malformed]\nactiveViewIndex: 0\ncustomPresets: []\n";
      auto const decodedRes = decodeWorkspaceSessionYaml(source);

      REQUIRE_FALSE(decodedRes);
      CHECK(decodedRes.error().code == Error::Code::FormatRejected);
    }

    SECTION("Malformed sort sequence elements are rejected")
    {
      auto const* source = R"(
        presentationVersion: 3
        libraryId: 123e4567-e89b-12d3-a456-426614174000
        openViews:
          - listId: 10
            filterExpression: ""
            presentation:
              kind: builtin
              presetId: library
              group: none
              sort: [malformed]
              visibleFields: [title]
              redundantFields: []
        activeViewIndex: 0
        customPresets: []
      )";
      auto const decodedRes = decodeWorkspaceSessionYaml(source);

      REQUIRE_FALSE(decodedRes);
      CHECK(decodedRes.error().code == Error::Code::FormatRejected);
    }

    SECTION("Malformed custom-preset sequence elements are rejected")
    {
      auto const* source = "presentationVersion: 3\nlibraryId: 123e4567-e89b-12d3-a456-426614174000\nopenViews: "
                           "[]\nactiveViewIndex: 0\ncustomPresets: [malformed]\n";
      auto const decodedRes = decodeWorkspaceSessionYaml(source);

      REQUIRE_FALSE(decodedRes);
      CHECK(decodedRes.error().code == Error::Code::FormatRejected);
    }

    SECTION("Malformed nested entries reject the whole candidate")
    {
      auto const* source = R"(
        presentationVersion: 3
        libraryId: 123e4567-e89b-12d3-a456-426614174000
        openViews:
          - listId: 10
            filterExpression: ""
            presentation: malformed
        activeViewIndex: 0
        customPresets: []
      )";
      auto const decodedRes = decodeWorkspaceSessionYaml(source);

      REQUIRE_FALSE(decodedRes);
      CHECK(decodedRes.error().code == Error::Code::FormatRejected);
      CHECK(decodedRes.error().message.contains("presentation"));
    }
  }

  TEST_CASE("WorkspaceSessionYamlSchema - rejects active view indexes outside uint32 representation")
  {
    SECTION("Negative active view index is rejected as unsigned 32-bit state")
    {
      auto const* source = "presentationVersion: 3\nlibraryId: 123e4567-e89b-12d3-a456-426614174000\nopenViews: "
                           "[]\nactiveViewIndex: -1\ncustomPresets: []\n";
      auto const decodedRes = decodeWorkspaceSessionYaml(source);

      REQUIRE_FALSE(decodedRes);
      CHECK(decodedRes.error().code == Error::Code::FormatRejected);
      CHECK(decodedRes.error().message.contains("workspace.activeViewIndex"));
      CHECK(decodedRes.error().message.contains("must be a valid scalar"));
    }

    SECTION("Active view index wider than unsigned 32-bit is rejected")
    {
      auto const* source = "presentationVersion: 3\nlibraryId: 123e4567-e89b-12d3-a456-426614174000\nopenViews: "
                           "[]\nactiveViewIndex: 4294967296\ncustomPresets: []\n";
      auto const decodedRes = decodeWorkspaceSessionYaml(source);

      REQUIRE_FALSE(decodedRes);
      CHECK(decodedRes.error().code == Error::Code::FormatRejected);
      CHECK(decodedRes.error().message.contains("workspace.activeViewIndex"));
      CHECK(decodedRes.error().message.contains("must be a valid scalar"));
    }
  }
} // namespace ao::rt::test
