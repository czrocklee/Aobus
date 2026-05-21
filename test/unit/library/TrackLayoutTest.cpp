// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "ao/library/TrackLayout.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>

namespace ao::library::test
{
  TEST_CASE("TrackHotHeader - Size and Alignment")
  {
    CHECK(sizeof(TrackHotHeader) == 36);
    CHECK(alignof(TrackHotHeader) == 4);
  }

  TEST_CASE("TrackHotHeader - Field Offsets")
  {
    // Check 4-byte section
    CHECK(offsetof(TrackHotHeader, tagBloom) == 0);
    CHECK(offsetof(TrackHotHeader, artistId) == 4);
    CHECK(offsetof(TrackHotHeader, albumId) == 8);
    CHECK(offsetof(TrackHotHeader, genreId) == 12);
    CHECK(offsetof(TrackHotHeader, albumArtistId) == 16);
    CHECK(offsetof(TrackHotHeader, composerId) == 20);

    // Check 2-byte section starts at offset 24
    CHECK(offsetof(TrackHotHeader, year) == 24);
    CHECK(offsetof(TrackHotHeader, codecId) == 26);
    CHECK(offsetof(TrackHotHeader, bitDepth) == 28);
    CHECK(offsetof(TrackHotHeader, titleLen) == 30);
    CHECK(offsetof(TrackHotHeader, tagLen) == 32);

    // Check 1-byte section
    CHECK(offsetof(TrackHotHeader, rating) == 34);
  }

  TEST_CASE("TrackColdHeader - Size and Alignment")
  {
    CHECK(sizeof(TrackColdHeader) == 36);
    CHECK(alignof(TrackColdHeader) == 4);
  }

  TEST_CASE("TrackColdHeader - Field Offsets")
  {
    // 4-byte section
    CHECK(offsetof(TrackColdHeader, durationMs) == 0);
    CHECK(offsetof(TrackColdHeader, sampleRate) == 4);
    CHECK(offsetof(TrackColdHeader, coverArtId) == 8);
    CHECK(offsetof(TrackColdHeader, bitrate) == 12);
    CHECK(offsetof(TrackColdHeader, workId) == 16);

    // 2-byte section
    CHECK(offsetof(TrackColdHeader, trackNumber) == 20);
    CHECK(offsetof(TrackColdHeader, totalTracks) == 22);
    CHECK(offsetof(TrackColdHeader, discNumber) == 24);
    CHECK(offsetof(TrackColdHeader, totalDiscs) == 26);
    CHECK(offsetof(TrackColdHeader, customCount) == 28);
    CHECK(offsetof(TrackColdHeader, uriOffset) == 30);
    CHECK(offsetof(TrackColdHeader, uriLen) == 32);

    // 1-byte section
    CHECK(offsetof(TrackColdHeader, channels) == 34);
  }
} // namespace ao::library::test
