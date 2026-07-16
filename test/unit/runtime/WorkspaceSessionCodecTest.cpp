// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "runtime/WorkspaceSessionCodec.h"

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/WorkspaceSessionState.h>

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

  TEST_CASE("WorkspaceSessionCodec - round-trip uses stable presentation vocabulary",
            "[runtime][unit][workspace][session-codec]")
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
      .activeListId = ListId{10},
      .customPresets =
        {
          CustomTrackPresentationPreset{
            .label = "Descending Albums",
            .basePresetId = "albums",
            .spec = presentation,
          },
        },
    };

    auto const document = detail::encodeWorkspaceSession(state);

    REQUIRE(document);
    CHECK(document->presentationVersion == 1);
    REQUIRE(document->openViews.size() == 1);
    auto const& stored = document->openViews[0].presentation;
    CHECK(stored.group == "album");
    REQUIRE(stored.sort.size() == 2);
    CHECK(stored.sort[0].field == "disc-number");
    CHECK(stored.sort[0].direction == "ascending");
    CHECK(stored.sort[1].field == "title");
    CHECK(stored.sort[1].direction == "descending");
    CHECK(stored.visibleFields == std::vector<std::string>{"title", "duration"});
    CHECK(stored.redundantFields == std::vector<std::string>{"album"});

    auto const decoded = detail::decodeWorkspaceSession(*document);

    REQUIRE(decoded);
    REQUIRE(decoded->openViews.size() == 1);
    REQUIRE(decoded->openViews[0].optPresentation);
    CHECK(*decoded->openViews[0].optPresentation == presentation);
    CHECK(decoded->openViews[0].groupBy == presentation.groupBy);
    CHECK(decoded->openViews[0].sortBy == presentation.sortBy);
    CHECK(decoded->activeListId == ListId{10});
    REQUIRE(decoded->customPresets.size() == 1);
    CHECK(decoded->customPresets[0] == state.customPresets[0]);
  }

  TEST_CASE("WorkspaceSessionCodec - canonicalizes permitted live presentation state",
            "[runtime][unit][workspace][session-codec]")
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

    auto const document = detail::encodeWorkspaceSession(state);

    REQUIRE(document);
    REQUIRE(document->openViews.size() == 2);
    CHECK(document->openViews[0].presentation.visibleFields == std::vector<std::string>{"title"});
    CHECK(document->openViews[1].presentation.visibleFields == std::vector<std::string>{"title"});
    CHECK(document->openViews[1].presentation.redundantFields == std::vector<std::string>{"album"});
  }

  TEST_CASE("WorkspaceSessionCodec - rejects invalid presentation objects", "[runtime][unit][workspace][session-codec]")
  {
    auto const validState = WorkspaceSessionState{
      .openViews =
        {
          TrackListViewConfig{.listId = ListId{10}, .optPresentation = makePresentation()},
        },
    };
    auto documentResult = detail::encodeWorkspaceSession(validState);
    REQUIRE(documentResult);
    auto document = std::move(*documentResult);
    auto& presentation = document.openViews[0].presentation;

    SECTION("Unsupported version")
    {
      document.presentationVersion = 2;
    }

    SECTION("Invalid list id")
    {
      document.openViews[0].listId = kInvalidListId.raw();
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

    auto const result = detail::decodeWorkspaceSession(document);

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::FormatRejected);
  }

  TEST_CASE("WorkspaceSessionCodec - refuses to encode invalid live state", "[runtime][unit][workspace][session-codec]")
  {
    auto state = WorkspaceSessionState{
      .openViews =
        {
          TrackListViewConfig{.listId = ListId{10}, .optPresentation = makePresentation()},
        },
    };

    SECTION("Invalid list id")
    {
      state.openViews[0].listId = kInvalidListId;
    }

    SECTION("Missing exact presentation")
    {
      state.openViews[0].optPresentation.reset();
    }

    SECTION("Empty presentation id")
    {
      state.openViews[0].optPresentation->id.clear();
    }

    SECTION("Duplicate sort field")
    {
      state.openViews[0].optPresentation->sortBy.push_back(state.openViews[0].optPresentation->sortBy[0]);
    }

    SECTION("Invalid field enum")
    {
      state.openViews[0].optPresentation->visibleFields[0] = static_cast<TrackField>(255);
    }

    auto const result = detail::encodeWorkspaceSession(state);

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::InvalidState);
  }
} // namespace ao::rt::test
