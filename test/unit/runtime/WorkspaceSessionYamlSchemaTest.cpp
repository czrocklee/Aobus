// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "runtime/WorkspaceSessionYamlSchema.h"

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/WorkspaceSessionState.h>
#include <ao/yaml/RymlAdapter.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    TrackPresentationSpec makePresentation()
    {
      return TrackPresentationSpec{
        .id = "custom.descending-albums",
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

  TEST_CASE("WorkspaceSessionYamlSchema - round-trip uses stable presentation vocabulary",
            "[runtime][unit][workspace][session-schema]")
  {
    auto const presentation = makePresentation();
    auto secondPresentation = presentation;
    secondPresentation.id = "custom.artist-titles";
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
            .groupBy = presentation.groupBy,
            .sortBy = presentation.sortBy,
            .optPresentation = presentation,
          },
          TrackListViewConfig{
            .listId = ListId{11},
            .filterExpression = "$genre = \"Classical\"",
            .groupBy = secondPresentation.groupBy,
            .sortBy = secondPresentation.sortBy,
            .optPresentation = secondPresentation,
          },
        },
      .activeViewIndex = 1,
      .customPresets =
        {
          CustomTrackPresentationPreset{
            .label = "Descending Albums",
            .basePresetId = "albums",
            .spec = presentation,
          },
        },
    };

    auto const documentRes = detail::toWorkspaceSessionDocument(state);

    REQUIRE(documentRes);
    CHECK(documentRes->presentationVersion == 1);
    REQUIRE(documentRes->openViews.size() == 2);
    CHECK(documentRes->activeViewIndex == 1);
    auto const& firstStoredView = documentRes->openViews[0];
    CHECK(firstStoredView.listId == 10);
    CHECK(firstStoredView.filterExpression == "$genre = \"Jazz\"");
    auto const& firstStored = firstStoredView.presentation;
    CHECK(firstStored.id == "custom.descending-albums");
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
    CHECK(secondStored.id == "custom.artist-titles");
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
    CHECK(decodedRes->openViews[0].groupBy == presentation.groupBy);
    CHECK(decodedRes->openViews[0].sortBy == presentation.sortBy);
    REQUIRE(decodedRes->openViews[1].optPresentation);
    CHECK(decodedRes->openViews[1].listId == ListId{11});
    CHECK(decodedRes->openViews[1].filterExpression == "$genre = \"Classical\"");
    CHECK(*decodedRes->openViews[1].optPresentation == secondPresentation);
    CHECK(decodedRes->openViews[1].groupBy == secondPresentation.groupBy);
    CHECK(decodedRes->openViews[1].sortBy == secondPresentation.sortBy);
    CHECK(decodedRes->activeViewIndex == 1);
    REQUIRE(decodedRes->customPresets.size() == 1);
    CHECK(decodedRes->customPresets[0] == state.customPresets[0]);
  }

  TEST_CASE("WorkspaceSessionYamlSchema - empty workspace uses active view index zero",
            "[runtime][unit][workspace][session-schema]")
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

  TEST_CASE("WorkspaceSessionYamlSchema - canonicalizes permitted live presentation state",
            "[runtime][unit][workspace][session-schema]")
  {
    auto const duplicateFields = TrackPresentationSpec{
      .id = "custom.minimal",
      .visibleFields = {TrackField::Title, TrackField::Title},
      .redundantFields = {TrackField::Album, TrackField::Album},
    };
    auto const state = WorkspaceSessionState{
      .openViews =
        {
          TrackListViewConfig{
            .listId = ListId{10},
            .optPresentation = TrackPresentationSpec{.id = "custom.defaults"},
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
  }

  TEST_CASE("WorkspaceSessionYamlSchema - rejects invalid persisted state",
            "[runtime][unit][workspace][session-schema]")
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

    SECTION("Empty presentation id")
    {
      presentation.id.clear();
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

    SECTION("Invalid custom preset presentation")
    {
      document.customPresets.push_back(detail::StoredCustomTrackPresentationPreset{
        .label = "Invalid",
        .basePresetId = "albums",
        .spec = presentation,
      });
      document.customPresets[0].spec.id.clear();
    }

    auto const res = detail::workspaceSessionStateFromDocument(document);

    REQUIRE_FALSE(res);
    CHECK(res.error().code == expectedCode);
  }

  TEST_CASE("WorkspaceSessionYamlSchema - owns the exact YAML mapping", "[runtime][unit][workspace][session-schema]")
  {
    auto const presentation = makePresentation();
    auto const state = WorkspaceSessionState{
      .openViews =
        {
          TrackListViewConfig{
            .listId = ListId{10},
            .filterExpression = "$genre = \"Jazz\"",
            .groupBy = presentation.groupBy,
            .sortBy = presentation.sortBy,
            .optPresentation = presentation,
          },
        },
      .activeViewIndex = 0,
      .customPresets =
        {
          CustomTrackPresentationPreset{
            .label = "Descending Albums",
            .basePresetId = "albums",
            .spec = presentation,
          },
        },
    };
    auto tree = ryml::Tree{yaml::callbacks()};

    REQUIRE(detail::WorkspaceSessionYamlSchema{}.serialize(tree.rootref(), state));
    auto const encoded = ryml::emitrs_yaml<std::string>(tree);
    auto const versionPosition = encoded.find("presentationVersion:");
    auto const viewsPosition = encoded.find("openViews:");
    auto const activePosition = encoded.find("activeViewIndex:");
    auto const presetsPosition = encoded.find("customPresets:");
    REQUIRE(versionPosition != std::string::npos);
    REQUIRE(viewsPosition != std::string::npos);
    REQUIRE(activePosition != std::string::npos);
    REQUIRE(presetsPosition != std::string::npos);
    CHECK(tree.rootref().num_children() == 4);
    CHECK(versionPosition < viewsPosition);
    CHECK(viewsPosition < activePosition);
    CHECK(activePosition < presetsPosition);
    auto const root = tree.rootref();
    CHECK(yaml::scalarView(root["presentationVersion"]) == "1");
    CHECK(yaml::scalarView(root["activeViewIndex"]) == "0");

    REQUIRE(root["openViews"].num_children() == 1);
    auto const view = root["openViews"][0];
    CHECK(view.num_children() == 3);
    CHECK(view.has_child("listId"));
    CHECK(view.has_child("filterExpression"));
    CHECK(view.has_child("presentation"));
    auto const checkPresentationShape = [](ryml::ConstNodeRef const presentationNode)
    {
      CHECK(presentationNode.num_children() == 5);
      CHECK(presentationNode.has_child("id"));
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
    CHECK(yaml::scalarView(viewPresentation["sort"][0]["field"]) == "disc-number");

    REQUIRE(root["customPresets"].num_children() == 1);
    auto const preset = root["customPresets"][0];
    CHECK(preset.num_children() == 3);
    CHECK(preset.has_child("label"));
    CHECK(preset.has_child("basePresetId"));
    CHECK(preset.has_child("spec"));
    checkPresentationShape(preset["spec"]);

    auto const decodedRes = detail::WorkspaceSessionYamlSchema{}.deserialize(tree.rootref(), WorkspaceSessionState{});
    REQUIRE(decodedRes);
    REQUIRE(decodedRes->openViews.size() == 1);
    CHECK(decodedRes->openViews[0].listId == ListId{10});
    CHECK(decodedRes->openViews[0].optPresentation == state.openViews[0].optPresentation);
    CHECK(decodedRes->customPresets == state.customPresets);
  }

  TEST_CASE("WorkspaceSessionYamlSchema - future version takes precedence over malformed payload",
            "[runtime][unit][workspace][session-schema]")
  {
    auto const* source = "presentationVersion: 99\nopenViews: malformed\nactiveViewIndex: malformed\ncustomPresets: "
                         "malformed\nfuture: true\n";
    auto const decodedRes = decodeWorkspaceSessionYaml(source);

    REQUIRE_FALSE(decodedRes);
    CHECK(decodedRes.error().code == Error::Code::NotSupported);
  }

  TEST_CASE("WorkspaceSessionYamlSchema - rejects missing required YAML fields",
            "[runtime][unit][workspace][session-schema]")
  {
    SECTION("Missing required fields are rejected")
    {
      auto const* source = "presentationVersion: 1\nopenViews: []\nactiveViewIndex: 0\n";
      auto const decodedRes = decodeWorkspaceSessionYaml(source);

      REQUIRE_FALSE(decodedRes);
      CHECK(decodedRes.error().code == Error::Code::FormatRejected);
      CHECK(decodedRes.error().message.contains("customPresets"));
    }

    SECTION("Missing active view index is rejected")
    {
      auto const* source = "presentationVersion: 1\nopenViews: []\ncustomPresets: []\n";
      auto const decodedRes = decodeWorkspaceSessionYaml(source);

      REQUIRE_FALSE(decodedRes);
      CHECK(decodedRes.error().code == Error::Code::FormatRejected);
      CHECK(decodedRes.error().message.contains("activeViewIndex"));
    }
  }

  TEST_CASE("WorkspaceSessionYamlSchema - rejects unknown YAML keys recursively",
            "[runtime][unit][workspace][session-schema]")
  {
    SECTION("Unknown structural keys are rejected")
    {
      auto const* source =
        "presentationVersion: 1\nopenViews: []\nactiveViewIndex: 0\ncustomPresets: []\nfuture: true\n";
      auto const decodedRes = decodeWorkspaceSessionYaml(source);

      REQUIRE_FALSE(decodedRes);
      CHECK(decodedRes.error().code == Error::Code::FormatRejected);
      CHECK(decodedRes.error().message.contains("future"));
    }

    SECTION("Unknown view keys are rejected")
    {
      auto const* source = R"(
        presentationVersion: 1
        openViews:
          - listId: 10
            filterExpression: ""
            presentation:
              id: library
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
        presentationVersion: 1
        openViews:
          - listId: 10
            filterExpression: ""
            presentation:
              id: library
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
        presentationVersion: 1
        openViews:
          - listId: 10
            filterExpression: ""
            presentation:
              id: library
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
        presentationVersion: 1
        openViews: []
        activeViewIndex: 0
        customPresets:
          - label: Custom
            basePresetId: library
            spec:
              id: custom
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

  TEST_CASE("WorkspaceSessionYamlSchema - rejects malformed YAML node kinds",
            "[runtime][unit][workspace][session-schema]")
  {
    SECTION("Malformed view sequence elements are rejected")
    {
      auto const* source = "presentationVersion: 1\nopenViews: [malformed]\nactiveViewIndex: 0\ncustomPresets: []\n";
      auto const decodedRes = decodeWorkspaceSessionYaml(source);

      REQUIRE_FALSE(decodedRes);
      CHECK(decodedRes.error().code == Error::Code::FormatRejected);
    }

    SECTION("Malformed sort sequence elements are rejected")
    {
      auto const* source = R"(
        presentationVersion: 1
        openViews:
          - listId: 10
            filterExpression: ""
            presentation:
              id: library
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
      auto const* source = "presentationVersion: 1\nopenViews: []\nactiveViewIndex: 0\ncustomPresets: [malformed]\n";
      auto const decodedRes = decodeWorkspaceSessionYaml(source);

      REQUIRE_FALSE(decodedRes);
      CHECK(decodedRes.error().code == Error::Code::FormatRejected);
    }

    SECTION("Malformed nested entries reject the whole candidate")
    {
      auto const* source = R"(
        presentationVersion: 1
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

  TEST_CASE("WorkspaceSessionYamlSchema - rejects active view indexes outside uint32 representation",
            "[runtime][unit][workspace][session-schema]")
  {
    SECTION("Negative active view index is rejected as unsigned 32-bit state")
    {
      auto const* source = "presentationVersion: 1\nopenViews: []\nactiveViewIndex: -1\ncustomPresets: []\n";
      auto const decodedRes = decodeWorkspaceSessionYaml(source);

      REQUIRE_FALSE(decodedRes);
      CHECK(decodedRes.error().code == Error::Code::FormatRejected);
      CHECK(decodedRes.error().message.contains("workspace.activeViewIndex"));
      CHECK(decodedRes.error().message.contains("must be a valid scalar"));
    }

    SECTION("Active view index wider than unsigned 32-bit is rejected")
    {
      auto const* source = "presentationVersion: 1\nopenViews: []\nactiveViewIndex: 4294967296\ncustomPresets: []\n";
      auto const decodedRes = decodeWorkspaceSessionYaml(source);

      REQUIRE_FALSE(decodedRes);
      CHECK(decodedRes.error().code == Error::Code::FormatRejected);
      CHECK(decodedRes.error().message.contains("workspace.activeViewIndex"));
      CHECK(decodedRes.error().message.contains("must be a valid scalar"));
    }
  }
} // namespace ao::rt::test
