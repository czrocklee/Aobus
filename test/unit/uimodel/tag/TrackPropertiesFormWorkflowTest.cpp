// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/StateTypes.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackFieldValue.h>
#include <ao/uimodel/tag/TrackPropertiesFormWorkflow.h>
#include <ao/uimodel/track/TrackFieldFormatter.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <utility>

namespace ao::uimodel::tag::test
{
  namespace
  {
    rt::TrackFieldRawValue textRaw(std::string value)
    {
      return rt::TrackFieldRawValue{std::in_place_type<std::string>, std::move(value)};
    }

    rt::TrackFieldRawValue numberRaw(std::uint16_t value)
    {
      return rt::TrackFieldRawValue{std::in_place_type<std::uint16_t>, value};
    }

    track::TrackFieldEditValue textEdit(std::string value)
    {
      return track::TrackFieldEditValue{std::in_place_type<std::string>, std::move(value)};
    }

    track::TrackFieldEditValue numberEdit(std::uint16_t value)
    {
      return track::TrackFieldEditValue{std::in_place_type<std::uint16_t>, value};
    }
  } // namespace

  TEST_CASE("TrackPropertiesFormWorkflow merges multi-track field state", "[uimodel][unit][tag][properties]")
  {
    auto state = makeTrackPropertiesFormFieldState(rt::TrackField::Title, textRaw("Same"));

    CHECK_FALSE(state.mixed);
    CHECK_FALSE(mergeTrackPropertiesFormFieldState(state, textRaw("Same")));
    CHECK_FALSE(state.mixed);

    CHECK(mergeTrackPropertiesFormFieldState(state, textRaw("Different")));
    CHECK(state.mixed);
    CHECK_FALSE(mergeTrackPropertiesFormFieldState(state, textRaw("Another")));
    CHECK(state.mixed);
  }

  TEST_CASE("TrackPropertiesFormWorkflow writes changed metadata edits", "[uimodel][unit][tag][properties]")
  {
    auto patch = rt::MetadataPatch{};
    auto const titleState = makeTrackPropertiesFormFieldState(rt::TrackField::Title, textRaw("Old Title"));
    auto const yearState = makeTrackPropertiesFormFieldState(rt::TrackField::Year, numberRaw(1999));

    CHECK(writeTrackPropertiesFormEdit(patch, titleState, textEdit("New Title")));
    REQUIRE(patch.optTitle.has_value());
    CHECK(*patch.optTitle == "New Title");

    CHECK(writeTrackPropertiesFormEdit(patch, yearState, numberEdit(2024)));
    REQUIRE(patch.optYear.has_value());
    CHECK(*patch.optYear == 2024);
  }

  TEST_CASE("TrackPropertiesFormWorkflow skips unchanged mixed and incompatible edits",
            "[uimodel][unit][tag][properties]")
  {
    auto patch = rt::MetadataPatch{};

    auto const unchanged = makeTrackPropertiesFormFieldState(rt::TrackField::Title, textRaw("Title"));
    CHECK_FALSE(writeTrackPropertiesFormEdit(patch, unchanged, textEdit("Title")));
    CHECK_FALSE(patch.optTitle.has_value());

    auto mixed = makeTrackPropertiesFormFieldState(rt::TrackField::Title, textRaw("First"));
    REQUIRE(mergeTrackPropertiesFormFieldState(mixed, textRaw("Second")));
    CHECK_FALSE(writeTrackPropertiesFormEdit(patch, mixed, textEdit("Replacement")));
    CHECK_FALSE(patch.optTitle.has_value());

    auto const wrongVariant = makeTrackPropertiesFormFieldState(rt::TrackField::Year, numberRaw(2000));
    CHECK_FALSE(writeTrackPropertiesFormEdit(patch, wrongVariant, textEdit("not a number")));
    CHECK_FALSE(patch.optYear.has_value());
  }
} // namespace ao::uimodel::tag::test
