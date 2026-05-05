// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators_all.hpp>
#include <catch2/matchers/catch_matchers_all.hpp>

#include <ao/Type.h>
#include <ao/library/TrackLayout.h>
#include <cstddef>

#include <test/unit/library/TestUtils.h>

namespace
{
  using namespace test;
  using ao::library::TrackColdHeader;
  using ao::library::TrackHotHeader;

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
    CHECK(sizeof(TrackColdHeader) == 52);
    CHECK(alignof(TrackColdHeader) == 4);
  }

  TEST_CASE("TrackColdHeader - Field Offsets")
  {
    // 4-byte section (split from original int64)
    CHECK(offsetof(TrackColdHeader, fileSizeLo) == 0);
    CHECK(offsetof(TrackColdHeader, fileSizeHi) == 4);
    CHECK(offsetof(TrackColdHeader, mtimeLo) == 8);
    CHECK(offsetof(TrackColdHeader, mtimeHi) == 12);

    // 4-byte section
    CHECK(offsetof(TrackColdHeader, durationMs) == 16);
    CHECK(offsetof(TrackColdHeader, sampleRate) == 20);
    CHECK(offsetof(TrackColdHeader, coverArtId) == 24);
    CHECK(offsetof(TrackColdHeader, bitrate) == 28);
    CHECK(offsetof(TrackColdHeader, workId) == 32);

    // 2-byte section
    CHECK(offsetof(TrackColdHeader, trackNumber) == 36);
    CHECK(offsetof(TrackColdHeader, totalTracks) == 38);
    CHECK(offsetof(TrackColdHeader, discNumber) == 40);
    CHECK(offsetof(TrackColdHeader, totalDiscs) == 42);
    CHECK(offsetof(TrackColdHeader, customCount) == 44);
    CHECK(offsetof(TrackColdHeader, uriOffset) == 46);
    CHECK(offsetof(TrackColdHeader, uriLen) == 48);

    // 1-byte section
    CHECK(offsetof(TrackColdHeader, channels) == 50);
  }
} // anonymous namespace
