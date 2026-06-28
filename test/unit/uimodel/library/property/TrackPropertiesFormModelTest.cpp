// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/StateTypes.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackFieldValue.h>
#include <ao/uimodel/field/TrackFieldEditCodec.h>
#include <ao/uimodel/field/TrackFieldFormatter.h>
#include <ao/uimodel/library/property/TrackPropertiesFormModel.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <utility>

namespace ao::uimodel::test
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

    TrackFieldEditValue textEdit(std::string value)
    {
      return TrackFieldEditValue{std::in_place_type<std::string>, std::move(value)};
    }

    TrackFieldEditValue numberEdit(std::uint16_t value)
    {
      return TrackFieldEditValue{std::in_place_type<std::uint16_t>, value};
    }
  } // namespace

  TEST_CASE("TrackPropertiesFormModel merges multi-track field state", "[uimodel][unit][library][property]")
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

  TEST_CASE("TrackPropertiesFormModel writes changed metadata edits", "[uimodel][unit][library][property]")
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

  TEST_CASE("TrackPropertiesFormModel exposes row view and save state", "[uimodel][unit][library][property]")
  {
    auto model = TrackPropertiesFormModel{};
    model.addField(rt::TrackField::Title, true);
    model.addField(rt::TrackField::FilePath, false);

    model.loadFirstTrackField(rt::TrackField::Title, textRaw("Old Title"));
    model.loadFirstTrackField(rt::TrackField::FilePath, textRaw("/music/old.flac"));

    auto const initialTitle = model.rowView(rt::TrackField::Title);
    CHECK(initialTitle.text == "Old Title");
    CHECK(initialTitle.editable);
    CHECK_FALSE(initialTitle.mixed);
    CHECK_FALSE(initialTitle.dirty);
    CHECK_FALSE(model.canSave());

    model.setEditValue(rt::TrackField::Title, textEdit("New Title"));

    auto const editedTitle = model.rowView(rt::TrackField::Title);
    CHECK(editedTitle.dirty);
    CHECK(model.canSave());

    auto const patch = model.buildPatch();
    REQUIRE(patch.optTitle.has_value());
    CHECK(*patch.optTitle == "New Title");
  }

  TEST_CASE("TrackPropertiesFormModel keeps mixed multi-track edits out of patches",
            "[uimodel][unit][library][property]")
  {
    auto model = TrackPropertiesFormModel{};
    model.addField(rt::TrackField::Title, true);

    model.loadFirstTrackField(rt::TrackField::Title, textRaw("First"));
    CHECK(model.mergeTrackField(rt::TrackField::Title, textRaw("Second")));

    auto const view = model.rowView(rt::TrackField::Title);
    CHECK(view.mixed);
    CHECK(view.text == kMultipleTrackValuesText);
    CHECK_FALSE(view.dirty);

    model.setEditValue(rt::TrackField::Title, textEdit("Replacement"));
    CHECK_FALSE(model.canSave());

    auto const patch = model.buildPatch();
    CHECK_FALSE(patch.optTitle.has_value());
  }

  TEST_CASE("TrackPropertiesFormModel skips unchanged mixed and incompatible edits",
            "[uimodel][unit][library][property]")
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
} // namespace ao::uimodel::test
