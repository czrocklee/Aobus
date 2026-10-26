// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/library/detail/TrackCustomMetadata.h>

#include <ao/rt/TrackField.h>
#include <ao/rt/projection/TrackDetailSnapshot.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>

namespace ao::uimodel::test
{
  namespace
  {
    constexpr auto kMixedText = "Mehrere Werte";
  }

  TEST_CASE("formatTrackCustomMetadataDisplayText formats aggregate custom metadata",
            "[uimodel][unit][library][detail]")
  {
    CHECK(formatTrackCustomMetadataDisplayText(
            rt::CustomMetadataItem{.key = "Mood", .value = {.optValue = "Bright"}}, kMixedText) == "Bright");
    CHECK(formatTrackCustomMetadataDisplayText(rt::CustomMetadataItem{.key = "Mood"}, kMixedText).empty());
    CHECK(formatTrackCustomMetadataDisplayText(
            rt::CustomMetadataItem{.key = "Mood", .value = {.mixed = true}}, kMixedText) == kMixedText);
  }

  TEST_CASE("isProtectedTrackCustomMetadataEditText protects aggregate sentinel text",
            "[uimodel][unit][library][detail]")
  {
    CHECK(isProtectedTrackCustomMetadataEditText(kMixedText, kMixedText));
    CHECK_FALSE(isProtectedTrackCustomMetadataEditText("", kMixedText));
    CHECK_FALSE(isProtectedTrackCustomMetadataEditText("edited", kMixedText));
  }

  TEST_CASE("validateCustomMetadataAddition reserves exact field ids and credits only",
            "[uimodel][unit][library][detail]")
  {
    auto snap = rt::TrackDetailSnapshot{};
    snap.customMetadata.push_back(rt::CustomMetadataItem{.key = "Mood"});

    CHECK(validateCustomMetadataAddition(snap, "Mood") == CustomMetadataAddValidation::DuplicateCustomMetadata);
    CHECK(validateCustomMetadataAddition(snap, "ReplayGain") == CustomMetadataAddValidation::Accepted);
    CHECK(validateCustomMetadataAddition(snap, "source") == CustomMetadataAddValidation::Accepted);
    CHECK(validateCustomMetadataAddition(snap, "credits") == CustomMetadataAddValidation::ReservedKey);
    CHECK(validateCustomMetadataAddition(snap, "musicians") == CustomMetadataAddValidation::Accepted);

    for (auto const& definition : rt::trackFieldDefinitions())
    {
      CAPTURE(definition.id);
      CHECK(validateCustomMetadataAddition(snap, definition.id) == CustomMetadataAddValidation::ReservedKey);
    }

    // Case and query-alias spellings are different keys. They are not reserved.
    for (auto const* const accepted :
         {"Title", "MUSICIANS", "Recording-Date", "albumArtist", "recordingDate", "trackNumber", "t", "aa", "musician"})
    {
      CAPTURE(accepted);
      CHECK(validateCustomMetadataAddition(snap, accepted) == CustomMetadataAddValidation::Accepted);
    }
  }

  TEST_CASE("validateCustomMetadataAddition prefers an existing custom key over reservation",
            "[uimodel][unit][library][detail]")
  {
    auto snap = rt::TrackDetailSnapshot{};
    snap.customMetadata.push_back(rt::CustomMetadataItem{.key = "title"});
    snap.customMetadata.push_back(rt::CustomMetadataItem{.key = "credits"});

    CHECK(validateCustomMetadataAddition(snap, "title") == CustomMetadataAddValidation::DuplicateCustomMetadata);
    CHECK(validateCustomMetadataAddition(snap, "credits") == CustomMetadataAddValidation::DuplicateCustomMetadata);
    CHECK(validateCustomMetadataAddition(snap, "artist") == CustomMetadataAddValidation::ReservedKey);
  }

  TEST_CASE("TrackCustomMetadata - patch helpers write update and delete payloads", "[uimodel][unit][library][detail]")
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
