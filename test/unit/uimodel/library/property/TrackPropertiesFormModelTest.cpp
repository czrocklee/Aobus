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
    CHECK_FALSE(model.tryMergeTrackField(rt::TrackField::Title, textRaw("Same")));
    CHECK_FALSE(model.rowView(rt::TrackField::Title).mixed);

    // Only the transition into mixed is reported; later disagreements are not.
    CHECK(model.tryMergeTrackField(rt::TrackField::Title, textRaw("Different")));
    CHECK(model.rowView(rt::TrackField::Title).mixed);
    CHECK_FALSE(model.tryMergeTrackField(rt::TrackField::Title, textRaw("Another")));
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
    CHECK(model.tryMergeTrackField(rt::TrackField::Title, textRaw("Second")));

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

  TEST_CASE("TrackPropertiesFormModel - explicit replacement writes mixed fields including first-target equality",
            "[uimodel][unit][library][property]")
  {
    auto model = TrackPropertiesFormModel{ao::test::englishMessageCatalog()};
    model.addField(rt::TrackField::Title, true);
    model.addField(rt::TrackField::Album, true);
    model.addField(rt::TrackField::Year, true);

    model.loadFirstTrackField(rt::TrackField::Title, textRaw("First"));
    model.loadFirstTrackField(rt::TrackField::Album, textRaw("Old"));
    model.loadFirstTrackField(rt::TrackField::Year, numberRaw(2001));
    CHECK(model.tryMergeTrackField(rt::TrackField::Title, textRaw("Second")));
    CHECK_FALSE(model.tryMergeTrackField(rt::TrackField::Album, textRaw("Old")));
    CHECK(model.tryMergeTrackField(rt::TrackField::Year, numberRaw(2002)));

    model.setExplicitFieldEdit(rt::TrackField::Title, textEdit("First"));
    model.setExplicitFieldEdit(rt::TrackField::Album, textEdit("New"));
    model.setExplicitFieldEdit(rt::TrackField::Year, numberEdit(0));

    CHECK(model.canSave());

    auto const patch = model.buildPatch();
    REQUIRE(patch.optTitle);
    CHECK(*patch.optTitle == "First");
    REQUIRE(patch.optAlbum);
    CHECK(*patch.optAlbum == "New");
    REQUIRE(patch.optYear);
    CHECK(*patch.optYear == 0);
  }

  TEST_CASE("TrackPropertiesFormModel - explicit replacement omits a common no-op and rejects read-only fields",
            "[uimodel][unit][library][property]")
  {
    auto model = TrackPropertiesFormModel{ao::test::englishMessageCatalog()};
    model.addField(rt::TrackField::Title, true);
    model.addField(rt::TrackField::Artist, false);

    model.loadFirstTrackField(rt::TrackField::Title, textRaw("Same"));
    model.loadFirstTrackField(rt::TrackField::Artist, textRaw("Original Artist"));
    CHECK_FALSE(model.tryMergeTrackField(rt::TrackField::Title, textRaw("Same")));

    model.setExplicitFieldEdit(rt::TrackField::Title, textEdit("Same"));
    model.setExplicitFieldEdit(rt::TrackField::Artist, textEdit("Changed Artist"));

    CHECK_FALSE(model.canSave());

    auto const patch = model.buildPatch();
    CHECK_FALSE(patch.optTitle);
    CHECK_FALSE(patch.optArtist);
  }

  TEST_CASE("TrackPropertiesFormModel - later edits replace explicit intent without changing the baseline",
            "[uimodel][regression][library][property]")
  {
    auto model = TrackPropertiesFormModel{ao::test::englishMessageCatalog()};
    model.addField(rt::TrackField::Title, true);
    model.loadFirstTrackField(rt::TrackField::Title, textRaw("Original"));
    model.setExplicitFieldEdit(rt::TrackField::Title, textEdit("Explicit"));
    REQUIRE(model.buildPatch().optTitle == "Explicit");

    SECTION("An ordinary edit replaces the previous explicit value")
    {
      model.setEditValue(rt::TrackField::Title, textEdit("Later"));
      CHECK(model.buildPatch().optTitle == "Later");
      CHECK(model.rowView(rt::TrackField::Title).text == "Original");
    }

    SECTION("Restoring a common value clears the patch")
    {
      model.setEditValue(rt::TrackField::Title, textEdit("Original"));
      CHECK_FALSE(model.canSave());
      CHECK_FALSE(model.buildPatch().optTitle);
    }

    SECTION("An ordinary edit resumes preservation of a mixed baseline")
    {
      REQUIRE(model.tryMergeTrackField(rt::TrackField::Title, textRaw("Different")));
      model.setEditValue(rt::TrackField::Title, textEdit("Later"));
      CHECK_FALSE(model.canSave());
      CHECK_FALSE(model.buildPatch().optTitle);
      CHECK(model.rowView(rt::TrackField::Title).mixed);
    }
  }
} // namespace ao::uimodel::test
