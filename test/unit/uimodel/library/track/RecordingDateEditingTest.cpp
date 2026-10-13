// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/unit/MessageCatalogTestSupport.h"
#include <ao/library/RecordingDate.h>
#include <ao/rt/TrackMutation.h>
#include <ao/uimodel/field/TrackFieldFormatter.h>
#include <ao/uimodel/library/property/TrackPropertiesFormModel.h>
#include <ao/uimodel/library/property/TrackPropertiesFormSpec.h>
#include <ao/uimodel/library/track/TrackAuthoring.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string_view>

namespace ao::uimodel::test
{
  TEST_CASE("RecordingDateEditing - editor syntax trims only the boundary and retains exact precision",
            "[uimodel][unit][recording-date]")
  {
    for (auto const text : std::array<std::string_view, 3>{"1981", "1981-05", "1981-05-12"})
    {
      auto const editRes = parseRecordingDateEditValue(text);
      REQUIRE(editRes);
      auto patch = rt::MetadataPatch{};
      REQUIRE(tryWriteTrackFieldPatch(patch, rt::TrackField::RecordingDate, *editRes));
      REQUIRE(patch.optRecordingDate);
      CHECK(library::formatRecordingDate(*patch.optRecordingDate) == text);
      CHECK(formatTrackFieldRawValue(ao::test::englishMessageCatalog(),
                                     rt::TrackField::RecordingDate,
                                     rt::TrackFieldRawValue{*patch.optRecordingDate}) == text);
    }

    auto const trimmedRes = parseRecordingDateEditValue(" \t1981-05-12\r\n");
    REQUIRE(trimmedRes);
    CHECK(std::get<library::RecordingDate>(*trimmedRes) == library::RecordingDate{1981, 5, 12});
    auto const clearRes = parseRecordingDateEditValue(" \t\n");
    REQUIRE(clearRes);
    CHECK(std::get<library::RecordingDate>(*clearRes) == library::RecordingDate{});

    for (auto const text :
         std::array<std::string_view, 6>{"0000", "1981-5", "1981-02-29", "1981 -05", "1981-00", "1981-05-12x"})
    {
      CHECK_FALSE(parseRecordingDateEditValue(text));
    }

    auto patch = rt::MetadataPatch{};
    CHECK_FALSE(tryWriteTrackFieldPatch(patch, rt::TrackField::RecordingDate, makeTextEditValue("1981")));
    CHECK_FALSE(tryWriteTrackFieldPatch(
      patch, rt::TrackField::RecordingDate, TrackFieldEditValue{library::RecordingDate{0, 1, 0}}));
    CHECK_FALSE(patch.optRecordingDate);
  }

  TEST_CASE("RecordingDateEditing - property drafts compare exact dates and explicitly replace mixed precision",
            "[uimodel][unit][recording-date]")
  {
    auto form = TrackPropertiesFormModel{ao::test::englishMessageCatalog()};
    form.addField(rt::TrackField::RecordingDate, true);
    form.loadFirstTrackField(rt::TrackField::RecordingDate, rt::TrackFieldRawValue{library::RecordingDate{1981, 0, 0}});
    CHECK(form.rowView(rt::TrackField::RecordingDate).text == "1981");
    CHECK_FALSE(form.canSave());
    form.setEditValue(rt::TrackField::RecordingDate, TrackFieldEditValue{library::RecordingDate{1981, 5, 12}});
    CHECK(form.canSave());
    CHECK(form.buildPatch().optRecordingDate == library::RecordingDate{1981, 5, 12});
    CHECK(form.tryMergeTrackField(
      rt::TrackField::RecordingDate, rt::TrackFieldRawValue{library::RecordingDate{1981, 5, 12}}));
    CHECK(form.rowView(rt::TrackField::RecordingDate).mixed);
    CHECK_FALSE(form.buildPatch().optRecordingDate);
    form.setExplicitFieldEdit(rt::TrackField::RecordingDate, TrackFieldEditValue{library::RecordingDate{1981, 0, 0}});
    CHECK(form.buildPatch().optRecordingDate == library::RecordingDate{1981, 0, 0});
    form.setExplicitFieldEdit(rt::TrackField::RecordingDate, TrackFieldEditValue{library::RecordingDate{}});
    REQUIRE(form.buildPatch().optRecordingDate);
    CHECK_FALSE(form.buildPatch().optRecordingDate->isPresent());
  }

  TEST_CASE("RecordingDateEditing - property specification uses a typed date editor and absent clear is unchanged",
            "[uimodel][unit][recording-date]")
  {
    auto const spec = buildTrackPropertiesFormSpec(ao::test::englishMessageCatalog());
    bool found = false;

    for (auto const& row : spec.metadataRows)
    {
      if (row.field == rt::TrackField::RecordingDate)
      {
        found = true;
        CHECK(row.editorKind == TrackPropertiesFormEditorKind::Date);
        CHECK(row.label == "Recording Date");
      }
    }

    CHECK(found);
    auto form = TrackPropertiesFormModel{ao::test::englishMessageCatalog()};
    form.addField(rt::TrackField::RecordingDate, true);
    form.loadFirstTrackField(rt::TrackField::RecordingDate, rt::TrackFieldRawValue{});
    form.setEditValue(rt::TrackField::RecordingDate, TrackFieldEditValue{library::RecordingDate{}});
    CHECK_FALSE(form.canSave());
    CHECK_FALSE(form.buildPatch().optRecordingDate);
  }
} // namespace ao::uimodel::test
