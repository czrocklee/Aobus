// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/library/TrackLayout.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>

namespace ao::library::test
{
  TEST_CASE("TrackHotHeader - Size and Alignment", "[library][unit][track]")
  {
    CHECK(sizeof(TrackHotHeader) == 40);
    CHECK(alignof(TrackHotHeader) == 4);
  }

  TEST_CASE("TrackHotHeader - Field Offsets", "[library][unit][track]")
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
    CHECK(offsetof(TrackHotHeader, bitDepth) == 30);
    CHECK(offsetof(TrackHotHeader, titleLen) == 32);
    CHECK(offsetof(TrackHotHeader, tagLen) == 34);

    // Check 1-byte section
    CHECK(offsetof(TrackHotHeader, codec) == 36);
    CHECK(offsetof(TrackHotHeader, rating) == 37);
  }

  TEST_CASE("TrackColdHeader - Size and Alignment", "[library][unit][track]")
  {
    CHECK(sizeof(TrackColdHeader) == 32);
    CHECK(alignof(TrackColdHeader) == 4);
  }

  TEST_CASE("TrackColdHeader - Field Offsets", "[library][unit][track]")
  {
    // 4-byte section
    CHECK(offsetof(TrackColdHeader, durationMs) == 0);
    CHECK(offsetof(TrackColdHeader, coverArtId) == 4);
    CHECK(offsetof(TrackColdHeader, bitrate) == 8);
    CHECK(offsetof(TrackColdHeader, workId) == 12);

    // 2-byte section
    CHECK(offsetof(TrackColdHeader, trackNumber) == 16);
    CHECK(offsetof(TrackColdHeader, totalTracks) == 18);
    CHECK(offsetof(TrackColdHeader, discNumber) == 20);
    CHECK(offsetof(TrackColdHeader, totalDiscs) == 22);
    CHECK(offsetof(TrackColdHeader, customCount) == 24);
    CHECK(offsetof(TrackColdHeader, uriOffset) == 26);
    CHECK(offsetof(TrackColdHeader, uriLen) == 28);

    // 1-byte section
    CHECK(offsetof(TrackColdHeader, channels) == 30);
  }
} // namespace ao::library::test
