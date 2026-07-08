// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/linux-gtk/track/TrackFieldUi.h"

#include <ao/rt/TrackField.h>
#include <ao/uimodel/field/TrackFieldEditPolicy.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

namespace ao::gtk::test
{
  using namespace ao::rt;

  namespace
  {
    auto const kAllFields = trackFieldDefinitions();
  } // namespace

  TEST_CASE("TrackFieldUi - registry exposes one adapter definition per runtime field", "[gtk][unit][track-field-ui]")
  {
    auto const uiDefs = trackFieldUiDefinitions();
    CHECK(uiDefs.size() == ao::rt::kTrackFieldCount);

    for (auto const& rtDef : kAllFields)
    {
      INFO("Field " << rtDef.id << " must have a UI definition");
      CHECK(trackFieldUiDefinition(rtDef.field) != nullptr);
    }
  }

  TEST_CASE("TrackFieldUi - registry wires inline-edit hooks for editable metadata fields",
            "[gtk][unit][track-field-ui]")
  {
    for (auto const& rtDef : kAllFields)
    {
      auto const* def = trackFieldUiDefinition(rtDef.field);

      INFO("Field: " << rtDef.id);
      REQUIRE(def != nullptr);

      auto const canWritePatch = uimodel::trackFieldCanWritePatch(rtDef.field);
      CHECK(canInlineEdit(*def) == canWritePatch);

      if (canWritePatch)
      {
        CHECK(def->parseInlineEdit != nullptr);
        CHECK(def->readRowEditValue != nullptr);
        CHECK(def->applyRowEditValue != nullptr);
      }
    }
  }

  TEST_CASE("TrackFieldUi - registry leaves future synthetic quality field read-only", "[gtk][unit][track-field-ui]")
  {
    auto const* def = trackFieldUiDefinition(TrackField::Quality);

    REQUIRE(def != nullptr);
    CHECK(def->readRowText != nullptr);
    CHECK_FALSE(canInlineEdit(*def));
  }
} // namespace ao::gtk::test
