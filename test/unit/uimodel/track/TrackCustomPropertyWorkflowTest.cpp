// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/projection/ProjectionTypes.h>
#include <ao/uimodel/track/TrackCustomPropertyWorkflow.h>
#include <ao/uimodel/track/TrackFieldFormatter.h>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>

namespace ao::uimodel::track::test
{
  TEST_CASE("displayTextForTrackCustomProperty formats aggregate custom metadata",
            "[uimodel][unit][track][custom-property]")
  {
    CHECK(displayTextForTrackCustomProperty(rt::CustomMetadataItem{.key = "Mood", .value = {.optValue = "Bright"}}) ==
          "Bright");
    CHECK(displayTextForTrackCustomProperty(rt::CustomMetadataItem{.key = "Mood"}).empty());
    CHECK(displayTextForTrackCustomProperty(rt::CustomMetadataItem{.key = "Mood", .value = {.mixed = true}}) ==
          kMultipleTrackValuesText);
  }

  TEST_CASE("isProtectedTrackCustomPropertyEditText protects aggregate sentinel text",
            "[uimodel][unit][track][custom-property]")
  {
    CHECK(isProtectedTrackCustomPropertyEditText(kMultipleTrackValuesText));
    CHECK_FALSE(isProtectedTrackCustomPropertyEditText(""));
    CHECK_FALSE(isProtectedTrackCustomPropertyEditText("edited"));
  }

  TEST_CASE("validateTrackCustomPropertyAddition rejects duplicate and reserved keys",
            "[uimodel][unit][track][custom-property]")
  {
    auto snap = rt::TrackDetailSnapshot{};
    snap.customMetadata.push_back(rt::CustomMetadataItem{.key = "Mood"});

    CHECK(validateTrackCustomPropertyAddition(snap, "ReplayGain") == TrackCustomPropertyAddValidation::Accepted);
    CHECK(validateTrackCustomPropertyAddition(snap, "Mood") ==
          TrackCustomPropertyAddValidation::DuplicateCustomProperty);
    CHECK(validateTrackCustomPropertyAddition(snap, "title") == TrackCustomPropertyAddValidation::ReservedTrackField);
  }

  TEST_CASE("undoValueForDeletedTrackCustomProperty returns safe restore values",
            "[uimodel][unit][track][custom-property]")
  {
    auto snap = rt::TrackDetailSnapshot{};
    snap.customMetadata.push_back(
      rt::CustomMetadataItem{.key = "All", .value = {.optValue = "same"}, .presentOnAll = true, .presentOnAny = true});
    snap.customMetadata.push_back(rt::CustomMetadataItem{
      .key = "Partial", .value = {.optValue = "value"}, .presentOnAll = false, .presentOnAny = true});
    snap.customMetadata.push_back(
      rt::CustomMetadataItem{.key = "Mixed", .value = {.mixed = true}, .presentOnAll = true, .presentOnAny = true});
    snap.customMetadata.push_back(
      rt::CustomMetadataItem{.key = "Empty", .value = {.optValue = ""}, .presentOnAll = true, .presentOnAny = true});

    CHECK(undoValueForDeletedTrackCustomProperty(snap, "All") == std::optional<std::string>{"same"});
    CHECK_FALSE(undoValueForDeletedTrackCustomProperty(snap, "Partial").has_value());
    CHECK_FALSE(undoValueForDeletedTrackCustomProperty(snap, "Mixed").has_value());
    CHECK(undoValueForDeletedTrackCustomProperty(snap, "Empty") == std::optional<std::string>{""});
    CHECK_FALSE(undoValueForDeletedTrackCustomProperty(snap, "Missing").has_value());
  }

  TEST_CASE("custom property patch helpers write update and delete payloads", "[uimodel][unit][track][custom-property]")
  {
    auto const updatePatch = makeTrackCustomPropertyUpdatePatch("Mood", "Bright");
    REQUIRE(updatePatch.customUpdates.size() == 1);
    REQUIRE(updatePatch.customUpdates.contains("Mood"));
    CHECK(updatePatch.customUpdates.at("Mood") == std::optional<std::string>{"Bright"});

    auto const deletePatch = makeTrackCustomPropertyDeletePatch("Mood");
    REQUIRE(deletePatch.customUpdates.size() == 1);
    REQUIRE(deletePatch.customUpdates.contains("Mood"));
    CHECK_FALSE(deletePatch.customUpdates.at("Mood").has_value());
  }
} // namespace ao::uimodel::track::test
