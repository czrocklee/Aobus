// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/Error.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/projection/TrackDetailProjection.h>
#include <ao/uimodel/field/TrackFieldEditCodec.h>
#include <ao/uimodel/field/TrackFieldFormatter.h>
#include <ao/uimodel/field/TrackInlineEditWorkflow.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace ao::uimodel::test
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

  TEST_CASE("TrackInlineEditWorkflow - applies inline metadata edits", "[uimodel][unit][workflow]")
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
        auto reply = rt::UpdateTrackMetadataReply{};
        reply.mutatedIds = {TrackId{1}};
        return reply;
      },
    };

    SECTION("no-op edits are ignored")
    {
      auto const result = TrackInlineEditWorkflow::apply(
        TrackInlineEditRequest{.field = rt::TrackField::Title, .oldText = "Old Title", .newText = "Old Title"}, hooks);

      CHECK(result.outcome == TrackInlineEditOutcome::NoChange);
      CHECK_FALSE(committedPatch.optTitle);
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
      REQUIRE(committedPatch.optTitle);
      CHECK(*committedPatch.optTitle == "New Title");
    }

    SECTION("a write that mutates nothing rolls back the optimistic edit")
    {
      hooks.commitPatch = [](rt::MetadataPatch const&) -> Result<rt::UpdateTrackMetadataReply>
      { return rt::UpdateTrackMetadataReply{}; };

      auto const result = TrackInlineEditWorkflow::apply(
        TrackInlineEditRequest{.field = rt::TrackField::Title, .oldText = "Old Title", .newText = "Temporary Title"},
        hooks);

      CHECK(result.outcome == TrackInlineEditOutcome::MutationRejected);
      CHECK(result.statusMessage == "Change could not be applied.");
      CHECK(textFrom(currentValue) == "Old Title");
    }

    SECTION("a rejected write rolls back the optimistic edit and surfaces the error")
    {
      hooks.commitPatch = [](rt::MetadataPatch const&) -> Result<rt::UpdateTrackMetadataReply>
      { return makeError(Error::Code::ValueTooLarge, "Metadata value is too large."); };

      auto const result = TrackInlineEditWorkflow::apply(
        TrackInlineEditRequest{.field = rt::TrackField::Title, .oldText = "Old Title", .newText = "Temporary Title"},
        hooks);

      CHECK(result.outcome == TrackInlineEditOutcome::MutationRejected);
      CHECK(result.statusMessage == "Metadata value is too large.");
      CHECK(textFrom(currentValue) == "Old Title");
    }
  }

  TEST_CASE("isProtectedInlineEditText protects aggregate sentinel values", "[uimodel][unit][field][inline-edit]")
  {
    auto snap = rt::TrackDetailSnapshot{};

    CHECK(isProtectedInlineEditText(rt::TrackField::Title, snap, kMultipleTrackValuesText, false));

    rt::trackFieldArrayAt(snap.fields, rt::TrackField::Title).optValue = std::string{"single"};
    CHECK(isProtectedInlineEditText(rt::TrackField::Title, snap, kMultipleTrackValuesText, false));

    rt::trackFieldArrayAt(snap.fields, rt::TrackField::Title).optValue.reset();
    rt::trackFieldArrayAt(snap.fields, rt::TrackField::Title).mixed = true;
    CHECK(isProtectedInlineEditText(rt::TrackField::Title, snap, kCompositeMixedTrackText, true));
    CHECK_FALSE(isProtectedInlineEditText(rt::TrackField::Title, snap, kCompositeMixedTrackText, false));
    CHECK_FALSE(isProtectedInlineEditText(rt::TrackField::Title, snap, "anything else", true));

    rt::trackFieldArrayAt(snap.fields, rt::TrackField::Title).mixed = false;
    rt::trackFieldArrayAt(snap.fields, rt::TrackField::Title).optValue = std::string{"single"};
    CHECK_FALSE(isProtectedInlineEditText(rt::TrackField::Title, snap, kCompositeMixedTrackText, true));
    CHECK_FALSE(isProtectedInlineEditText(rt::TrackField::Title, snap, "edit", true));
    CHECK_FALSE(isProtectedInlineEditText(rt::TrackField::Title, snap, "", true));
  }
} // namespace ao::uimodel::test
