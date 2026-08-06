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
  } // namespace

  TEST_CASE("WorkspaceSessionYamlSchema - round-trip uses stable presentation vocabulary",
            "[runtime][unit][workspace][session-schema]")
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
          TrackListViewConfig{
            .listId = ListId{11},
            .filterExpression = "$genre = \"Classical\"",
            .groupBy = presentation.groupBy,
            .sortBy = presentation.sortBy,
            .optPresentation = presentation,
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
    auto const& stored = documentRes->openViews[0].presentation;
    CHECK(stored.group == "album");
    REQUIRE(stored.sort.size() == 2);
    CHECK(stored.sort[0].field == "disc-number");
    CHECK(stored.sort[0].direction == "ascending");
    CHECK(stored.sort[1].field == "title");
    CHECK(stored.sort[1].direction == "descending");
    CHECK(stored.visibleFields == std::vector<std::string>{"title", "duration"});
    CHECK(stored.redundantFields == std::vector<std::string>{"album"});

    auto const decodedRes = detail::workspaceSessionStateFromDocument(*documentRes);

    REQUIRE(decodedRes);
    REQUIRE(decodedRes->openViews.size() == 2);
    REQUIRE(decodedRes->openViews[0].optPresentation);
    CHECK(*decodedRes->openViews[0].optPresentation == presentation);
    CHECK(decodedRes->openViews[0].groupBy == presentation.groupBy);
    CHECK(decodedRes->openViews[0].sortBy == presentation.sortBy);
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

    SECTION("Duplicate redundant field")
    {
      presentation.redundantFields.push_back(presentation.redundantFields[0]);
    }

    auto const result = detail::workspaceSessionStateFromDocument(document);

    REQUIRE_FALSE(result);
    CHECK(result.error().code == expectedCode);
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
    CHECK(yaml::scalarView(tree.rootref()["presentationVersion"]) == "1");
    CHECK(yaml::scalarView(tree.rootref()["activeViewIndex"]) == "0");
    CHECK(yaml::scalarView(tree.rootref()["openViews"][0]["presentation"]["sort"][0]["field"]) == "disc-number");

    auto const decodedRes = detail::WorkspaceSessionYamlSchema{}.deserialize(tree.rootref(), WorkspaceSessionState{});
    REQUIRE(decodedRes);
    REQUIRE(decodedRes->openViews.size() == 1);
    CHECK(decodedRes->openViews[0].listId == ListId{10});
    CHECK(decodedRes->openViews[0].optPresentation == state.openViews[0].optPresentation);
  }

  TEST_CASE("WorkspaceSessionYamlSchema - rejects invalid YAML candidates",
            "[runtime][unit][workspace][session-schema]")
  {
    SECTION("Future version is reported before interpreting its payload")
    {
      auto const* source = "presentationVersion: 99\nopenViews: malformed\nactiveViewIndex: malformed\ncustomPresets: "
                           "malformed\nfuture: true\n";
      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(source), &tree);
      auto const decodedRes = detail::WorkspaceSessionYamlSchema{}.deserialize(tree.rootref(), WorkspaceSessionState{});

      REQUIRE_FALSE(decodedRes);
      CHECK(decodedRes.error().code == Error::Code::NotSupported);
    }

    SECTION("Missing required fields are rejected")
    {
      auto const* source = "presentationVersion: 1\nopenViews: []\nactiveViewIndex: 0\n";
      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(source), &tree);
      auto const decodedRes = detail::WorkspaceSessionYamlSchema{}.deserialize(tree.rootref(), WorkspaceSessionState{});

      REQUIRE_FALSE(decodedRes);
      CHECK(decodedRes.error().code == Error::Code::FormatRejected);
      CHECK(decodedRes.error().message.contains("customPresets"));
    }

    SECTION("Missing active view index is rejected")
    {
      auto const* source = "presentationVersion: 1\nopenViews: []\ncustomPresets: []\n";
      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(source), &tree);
      auto const decodedRes = detail::WorkspaceSessionYamlSchema{}.deserialize(tree.rootref(), WorkspaceSessionState{});

      REQUIRE_FALSE(decodedRes);
      CHECK(decodedRes.error().code == Error::Code::FormatRejected);
      CHECK(decodedRes.error().message.contains("activeViewIndex"));
    }

    SECTION("Unknown structural keys are rejected")
    {
      auto const* source =
        "presentationVersion: 1\nopenViews: []\nactiveViewIndex: 0\ncustomPresets: []\nfuture: true\n";
      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(source), &tree);
      auto const decodedRes = detail::WorkspaceSessionYamlSchema{}.deserialize(tree.rootref(), WorkspaceSessionState{});

      REQUIRE_FALSE(decodedRes);
      CHECK(decodedRes.error().code == Error::Code::FormatRejected);
      CHECK(decodedRes.error().message.contains("future"));
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
      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(source), &tree);
      auto const decodedRes = detail::WorkspaceSessionYamlSchema{}.deserialize(tree.rootref(), WorkspaceSessionState{});

      REQUIRE_FALSE(decodedRes);
      CHECK(decodedRes.error().code == Error::Code::FormatRejected);
      CHECK(decodedRes.error().message.contains("presentation"));
    }
  }
} // namespace ao::rt::test
