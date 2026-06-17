// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/Error.h>
#include <ao/rt/StateTypes.h>
#include <ao/rt/TrackField.h>
#include <ao/uimodel/track/TrackFieldFormatter.h>
#include <ao/uimodel/track/TrackInlineEditWorkflow.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace ao::uimodel::track::test
{
  namespace
  {
    TrackFieldEditValue textValue(std::string_view value)
    {
      return TrackFieldEditValue{std::in_place_type<std::string>, std::string{value}};
    }

    std::string textFrom(TrackFieldEditValue const& value)
    {
      if (auto const* text = std::get_if<std::string>(&value); text != nullptr)
      {
        return *text;
      }

      return {};
    }
  } // namespace

  TEST_CASE("TrackInlineEditWorkflow - applies inline metadata edits", "[uimodel][track][inline-edit]")
  {
    auto currentValue = textValue("Old Title");
    auto appliedValue = std::string{};
    auto committedPatch = rt::MetadataPatch{};

    auto hooks = TrackInlineEditHooks{
      .parse = [](std::string_view text) -> Result<TrackFieldEditValue> { return textValue(text); },
      .readCurrentValue = [&] { return currentValue; },
      .applyValue =
        [&](TrackFieldEditValue const& value)
      {
        currentValue = value;
        appliedValue = textFrom(value);
      },
      .writePatch =
        [](rt::MetadataPatch& patch, TrackFieldEditValue const& value)
      {
        if (auto const* text = std::get_if<std::string>(&value); text != nullptr)
        {
          patch.optTitle = *text;
        }
      },
      .commitPatch = [&](rt::MetadataPatch const& patch) -> Result<rt::UpdateTrackMetadataReply>
      {
        committedPatch = patch;
        return rt::UpdateTrackMetadataReply{};
      },
    };

    SECTION("no-op edits are ignored")
    {
      auto const result = TrackInlineEditWorkflow::apply(
        TrackInlineEditRequest{.field = rt::TrackField::Title, .oldText = "Old Title", .newText = "Old Title"}, hooks);

      CHECK(result.outcome == TrackInlineEditOutcome::NoChange);
      CHECK_FALSE(committedPatch.optTitle.has_value());
    }

    SECTION("missing hooks report not editable")
    {
      auto const result = TrackInlineEditWorkflow::apply(
        TrackInlineEditRequest{.field = rt::TrackField::Duration, .oldText = "3:00", .newText = "4:00"}, {});

      CHECK(result.outcome == TrackInlineEditOutcome::NotEditable);
    }

    SECTION("parse errors surface status text without applying")
    {
      hooks.parse = [](std::string_view) -> Result<TrackFieldEditValue>
      { return makeError(Error::Code::FormatRejected, "Enter a whole number."); };

      auto const result = TrackInlineEditWorkflow::apply(
        TrackInlineEditRequest{.field = rt::TrackField::Year, .oldText = "1999", .newText = "bad"}, hooks);

      CHECK(result.outcome == TrackInlineEditOutcome::ParseRejected);
      CHECK(result.statusMessage == "Enter a whole number.");
      CHECK(textFrom(currentValue) == "Old Title");
      CHECK(appliedValue.empty());
    }

    SECTION("successful edits apply locally and commit patch")
    {
      auto const result = TrackInlineEditWorkflow::apply(
        TrackInlineEditRequest{.field = rt::TrackField::Title, .oldText = "Old Title", .newText = "New Title"}, hooks);

      CHECK(result.outcome == TrackInlineEditOutcome::Applied);
      CHECK(textFrom(currentValue) == "New Title");
      CHECK(appliedValue == "New Title");
      REQUIRE(committedPatch.optTitle.has_value());
      CHECK(*committedPatch.optTitle == "New Title");
    }

    SECTION("mutation errors roll back the optimistic edit")
    {
      hooks.commitPatch = [](rt::MetadataPatch const&) -> Result<rt::UpdateTrackMetadataReply>
      { return makeError(Error::Code::Generic, "metadata write failed"); };

      auto const result = TrackInlineEditWorkflow::apply(
        TrackInlineEditRequest{.field = rt::TrackField::Title, .oldText = "Old Title", .newText = "Temporary Title"},
        hooks);

      CHECK(result.outcome == TrackInlineEditOutcome::MutationRejected);
      CHECK(result.statusMessage == "metadata write failed");
      CHECK(textFrom(currentValue) == "Old Title");
    }
  }
} // namespace ao::uimodel::track::test
