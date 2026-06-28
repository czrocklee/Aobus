// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/library/TrackLayout.h>

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
    CHECK(sizeof(TrackColdHeader) == 40);
    CHECK(alignof(TrackColdHeader) == 4);
  }

  TEST_CASE("TrackColdHeader - stores fields at stable offsets", "[library][unit][track]")
  {
    // 4-byte section
    CHECK(offsetof(TrackColdHeader, duration) == 0);
    CHECK(offsetof(TrackColdHeader, bitrate) == 4);
    CHECK(offsetof(TrackColdHeader, workId) == 8);
    CHECK(offsetof(TrackColdHeader, movementId) == 12);

    // 2-byte section
    CHECK(offsetof(TrackColdHeader, trackNumber) == 16);
    CHECK(offsetof(TrackColdHeader, trackTotal) == 18);
    CHECK(offsetof(TrackColdHeader, discNumber) == 20);
    CHECK(offsetof(TrackColdHeader, discTotal) == 22);
    CHECK(offsetof(TrackColdHeader, movementNumber) == 24);
    CHECK(offsetof(TrackColdHeader, movementTotal) == 26);
    CHECK(offsetof(TrackColdHeader, customCount) == 28);
    CHECK(offsetof(TrackColdHeader, uriOffset) == 30);
    CHECK(offsetof(TrackColdHeader, uriLength) == 32);
    CHECK(offsetof(TrackColdHeader, coverCount) == 34);
    CHECK(offsetof(TrackColdHeader, customOffset) == 36);

    // 1-byte section
    CHECK(offsetof(TrackColdHeader, channels) == 38);
  }
} // namespace ao::library::test
