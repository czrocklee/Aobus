// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/library/property/TrackPropertiesFormModel.h>

#include "test/unit/MessageCatalogTestSupport.h"
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackFieldValue.h>
#include <ao/rt/TrackMutation.h>
#include <ao/uimodel/library/track/TrackAuthoring.h>

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

  TEST_CASE("TrackPropertiesFormModel - merges multi-track field state", "[uimodel][unit][library][property]")
  {
    auto model = TrackPropertiesFormModel{ao::test::englishMessageCatalog()};
    model.addField(rt::TrackField::Title, true);
    model.loadFirstTrackField(rt::TrackField::Title, textRaw("Same"));

    CHECK_FALSE(model.rowView(rt::TrackField::Title).mixed);
    CHECK_FALSE(model.mergeTrackField(rt::TrackField::Title, textRaw("Same")));
    CHECK_FALSE(model.rowView(rt::TrackField::Title).mixed);

    // Only the transition into mixed is reported; later disagreements are not.
    CHECK(model.mergeTrackField(rt::TrackField::Title, textRaw("Different")));
    CHECK(model.rowView(rt::TrackField::Title).mixed);
    CHECK_FALSE(model.mergeTrackField(rt::TrackField::Title, textRaw("Another")));
    CHECK(model.rowView(rt::TrackField::Title).mixed);
  }

  TEST_CASE("TrackPropertiesFormModel - writes changed metadata edits", "[uimodel][unit][library][property]")
  {
    auto model = TrackPropertiesFormModel{ao::test::englishMessageCatalog()};
    model.addField(rt::TrackField::Title, true);
    model.addField(rt::TrackField::Year, true);

    model.loadFirstTrackField(rt::TrackField::Title, textRaw("Old Title"));
    model.loadFirstTrackField(rt::TrackField::Year, numberRaw(1999));

    model.setEditValue(rt::TrackField::Title, textEdit("New Title"));
    model.setEditValue(rt::TrackField::Year, numberEdit(2024));

    CHECK(model.canSave());

    auto const patch = model.buildPatch();
    REQUIRE(patch.optTitle);
    CHECK(*patch.optTitle == "New Title");
    REQUIRE(patch.optYear);
    CHECK(*patch.optYear == 2024);
  }

  TEST_CASE("TrackPropertiesFormModel - exposes row view and save state", "[uimodel][unit][library][property]")
  {
    auto model = TrackPropertiesFormModel{ao::test::englishMessageCatalog()};
    model.addField(rt::TrackField::Title, true);
    model.addField(rt::TrackField::FilePath, false);

    model.loadFirstTrackField(rt::TrackField::Title, textRaw("Old Title"));
    model.loadFirstTrackField(rt::TrackField::FilePath, textRaw("/music/old.flac"));

    auto const initialTitle = model.rowView(rt::TrackField::Title);
    CHECK(initialTitle.text == "Old Title");
    CHECK(initialTitle.editable);
    CHECK_FALSE(initialTitle.mixed);
    CHECK_FALSE(model.canSave());

    model.setEditValue(rt::TrackField::Title, textEdit("New Title"));

    CHECK(model.canSave());

    auto const patch = model.buildPatch();
    REQUIRE(patch.optTitle);
    CHECK(*patch.optTitle == "New Title");
  }

  TEST_CASE("TrackPropertiesFormModel - keeps mixed multi-track edits out of patches",
            "[uimodel][unit][library][property]")
  {
    auto const& textCatalog = ao::test::englishMessageCatalog();
    auto model = TrackPropertiesFormModel{textCatalog};
    model.addField(rt::TrackField::Title, true);

    model.loadFirstTrackField(rt::TrackField::Title, textRaw("First"));
    CHECK(model.mergeTrackField(rt::TrackField::Title, textRaw("Second")));

    auto const view = model.rowView(rt::TrackField::Title);
    CHECK(view.mixed);
    CHECK(view.text == i18n::requiredText(textCatalog, i18n::MessageId::TrackMultipleValues));

    model.setEditValue(rt::TrackField::Title, textEdit("Replacement"));
    CHECK_FALSE(model.canSave());

    auto const patch = model.buildPatch();
    CHECK_FALSE(patch.optTitle);
  }

  TEST_CASE("TrackPropertiesFormModel - skips unchanged, read-only, and incompatible edits",
            "[uimodel][unit][library][property]")
  {
    auto model = TrackPropertiesFormModel{ao::test::englishMessageCatalog()};
    model.addField(rt::TrackField::Title, true);
    model.addField(rt::TrackField::Artist, false);
    model.addField(rt::TrackField::Year, true);

    model.loadFirstTrackField(rt::TrackField::Title, textRaw("Title"));
    model.loadFirstTrackField(rt::TrackField::Artist, textRaw("Artist"));
    model.loadFirstTrackField(rt::TrackField::Year, numberRaw(2000));

    // An edit equal to the loaded value, an edit to a read-only row, and an
    // edit whose variant does not match the field all leave the patch alone.
    model.setEditValue(rt::TrackField::Title, textEdit("Title"));
    model.setEditValue(rt::TrackField::Artist, textEdit("Another Artist"));
    model.setEditValue(rt::TrackField::Year, textEdit("not a number"));

    CHECK_FALSE(model.canSave());

    auto const patch = model.buildPatch();
    CHECK_FALSE(patch.optTitle);
    CHECK_FALSE(patch.optArtist);
    CHECK_FALSE(patch.optYear);
  }
} // namespace ao::uimodel::test
