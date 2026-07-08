// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/projection/TrackDetailProjection.h>
#include <ao/uimodel/field/TrackFieldFormatter.h>
#include <ao/uimodel/library/detail/TrackCustomMetadataWorkflow.h>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>

namespace ao::uimodel::test
{
  TEST_CASE("formatTrackCustomMetadataDisplayText formats aggregate custom metadata",
            "[uimodel][unit][library][detail]")
  {
    CHECK(formatTrackCustomMetadataDisplayText(
            rt::CustomMetadataItem{.key = "Mood", .value = {.optValue = "Bright"}}) == "Bright");
    CHECK(formatTrackCustomMetadataDisplayText(rt::CustomMetadataItem{.key = "Mood"}).empty());
    CHECK(formatTrackCustomMetadataDisplayText(rt::CustomMetadataItem{.key = "Mood", .value = {.mixed = true}}) ==
          kMultipleTrackValuesText);
  }

  TEST_CASE("isProtectedTrackCustomMetadataEditText protects aggregate sentinel text",
            "[uimodel][unit][library][detail]")
  {
    CHECK(isProtectedTrackCustomMetadataEditText(kMultipleTrackValuesText));
    CHECK_FALSE(isProtectedTrackCustomMetadataEditText(""));
    CHECK_FALSE(isProtectedTrackCustomMetadataEditText("edited"));
  }

  TEST_CASE("validateCustomMetadataAddition rejects duplicate and reserved keys", "[uimodel][unit][library][detail]")
  {
    auto snap = rt::TrackDetailSnapshot{};
    snap.customMetadata.push_back(rt::CustomMetadataItem{.key = "Mood"});

    CHECK(validateCustomMetadataAddition(snap, "ReplayGain") == CustomMetadataAddValidation::Accepted);
    CHECK(validateCustomMetadataAddition(snap, "Mood") == CustomMetadataAddValidation::DuplicateCustomMetadata);
    CHECK(validateCustomMetadataAddition(snap, "title") == CustomMetadataAddValidation::ReservedTrackField);
  }

  TEST_CASE("undoValueForDeletedTrackCustomMetadata returns safe restore values", "[uimodel][unit][library][detail]")
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

    CHECK(undoValueForDeletedTrackCustomMetadata(snap, "All") == std::optional<std::string>{"same"});
    CHECK_FALSE(undoValueForDeletedTrackCustomMetadata(snap, "Partial").has_value());
    CHECK_FALSE(undoValueForDeletedTrackCustomMetadata(snap, "Mixed").has_value());
    CHECK(undoValueForDeletedTrackCustomMetadata(snap, "Empty") == std::optional<std::string>{""});
    CHECK_FALSE(undoValueForDeletedTrackCustomMetadata(snap, "Missing").has_value());
  }

  TEST_CASE("custom - metadata patch helpers write update and delete payloads", "[uimodel][unit][library][detail]")
  {
    auto const updatePatch = makeCustomMetadataUpdatePatch("Mood", "Bright");
    REQUIRE(updatePatch.customUpdates.size() == 1);
    REQUIRE(updatePatch.customUpdates.contains("Mood"));
    CHECK(updatePatch.customUpdates.at("Mood") == std::optional<std::string>{"Bright"});

    auto const deletePatch = makeCustomMetadataDeletePatch("Mood");
    REQUIRE(deletePatch.customUpdates.size() == 1);
    REQUIRE(deletePatch.customUpdates.contains("Mood"));
    CHECK_FALSE(deletePatch.customUpdates.at("Mood").has_value());
  }
} // namespace ao::uimodel::test
