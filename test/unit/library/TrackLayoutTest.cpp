// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/library/TrackLayout.h>

#include <ao/library/RecordingDate.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>

namespace ao::library::test
{
  TEST_CASE("TrackHotHeader - has stable size and alignment", "[library][unit][track]")
  {
    CHECK(sizeof(TrackHotHeader) == 36);
    CHECK(alignof(TrackHotHeader) == 4);
  }

  TEST_CASE("TrackHotHeader - stores fields at stable offsets", "[library][unit][track]")
  {
    // Check 4-byte section
    CHECK(offsetof(TrackHotHeader, tagBloom) == 0);
    CHECK(offsetof(TrackHotHeader, artistId) == 4);
    CHECK(offsetof(TrackHotHeader, albumId) == 8);
    CHECK(offsetof(TrackHotHeader, genreId) == 12);
    CHECK(offsetof(TrackHotHeader, albumArtistId) == 16);
    CHECK(offsetof(TrackHotHeader, composerId) == 20);
    CHECK(offsetof(TrackHotHeader, sampleRate) == 24);

    // Check 2-byte section starts at offset 28
    CHECK(offsetof(TrackHotHeader, year) == 28);
    CHECK(offsetof(TrackHotHeader, titleLength) == 30);
    CHECK(offsetof(TrackHotHeader, tagLength) == 32);

    // Check 1-byte section
    CHECK(offsetof(TrackHotHeader, bitDepth) == 34);
    CHECK(offsetof(TrackHotHeader, codec) == 35);
  }

  TEST_CASE("TrackColdHeader - has stable size and alignment", "[library][unit][track]")
  {
    CHECK(sizeof(TrackColdHeader) == 32);
    CHECK(alignof(TrackColdHeader) == 4);
  }

  TEST_CASE("TrackColdHeader - stores fields at stable offsets", "[library][unit][track]")
  {
    // 4-byte section
    CHECK(offsetof(TrackColdHeader, duration) == 0);
    CHECK(offsetof(TrackColdHeader, bitrate) == 4);

    // 2-byte section
    CHECK(offsetof(TrackColdHeader, trackNumber) == 8);
    CHECK(offsetof(TrackColdHeader, trackTotal) == 10);
    CHECK(offsetof(TrackColdHeader, discNumber) == 12);
    CHECK(offsetof(TrackColdHeader, discTotal) == 14);
    CHECK(offsetof(TrackColdHeader, blockOffsets) == 16);
    CHECK(offsetof(TrackColdHeader, uriOffset) == 26);
    CHECK(offsetof(TrackColdHeader, uriLength) == 28);

    // 1-byte section
    CHECK(offsetof(TrackColdHeader, channels) == 30);
    CHECK(offsetof(TrackColdHeader, reserved8) == 31);
  }

  TEST_CASE("TrackCold extension blocks - have stable size and alignment", "[library][unit][track]")
  {
    CHECK(kTrackColdKnownBlockSlotCount == 4);
    CHECK(kTrackColdBlockSlotCount == 5);
    CHECK(trackColdBlockSlotIndex(TrackColdBlockSlot::CoverArt) == 0);
    CHECK(trackColdBlockSlotIndex(TrackColdBlockSlot::Work) == 1);
    CHECK(trackColdBlockSlotIndex(TrackColdBlockSlot::CustomMetadata) == 2);
    CHECK(sizeof(CoverArtEntry) == 8);
    CHECK(alignof(CoverArtEntry) == 4);
    CHECK(trackColdBlockSlotIndex(TrackColdBlockSlot::Performance) == 3);
    CHECK(sizeof(TrackWorkBlock) == 12);
    CHECK(alignof(TrackWorkBlock) == 4);
    CHECK(offsetof(TrackWorkBlock, workId) == 0);
    CHECK(offsetof(TrackWorkBlock, movementId) == 4);
    CHECK(offsetof(TrackWorkBlock, movementNumber) == 8);
    CHECK(offsetof(TrackWorkBlock, movementTotal) == 10);
    CHECK(sizeof(TrackPerformanceBlock) == 12);
    CHECK(alignof(TrackPerformanceBlock) == 4);
    CHECK(offsetof(TrackPerformanceBlock, recordingDate) == 0);
    CHECK(offsetof(TrackPerformanceBlock, sectionCounts) == 4);
    CHECK(offsetof(TrackPerformanceBlock, reserved) == 10);
    CHECK(offsetof(RecordingDate, year) == 0);
    CHECK(offsetof(RecordingDate, month) == 2);
    CHECK(offsetof(RecordingDate, day) == 3);
    CHECK(sizeof(TrackCreditEntry) == 8);
    CHECK(alignof(TrackCreditEntry) == 4);
    CHECK(offsetof(TrackCreditEntry, nameId) == 0);
    CHECK(offsetof(TrackCreditEntry, roleId) == 4);
    CHECK(sizeof(CustomMetadataBlockHeader) == 8);
    CHECK(alignof(CustomMetadataBlockHeader) <= 4);
    CHECK(sizeof(CustomMetadataEntry) == 8);
    CHECK(alignof(CustomMetadataEntry) == 4);
  }
} // namespace ao::library::test
