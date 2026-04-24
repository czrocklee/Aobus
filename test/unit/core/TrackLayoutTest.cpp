// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <catch2/catch.hpp>

#include <cstddef>
#include <rs/core/TrackLayout.h>
#include <rs/core/Type.h>

#include <test/unit/core/TestUtils.h>

namespace
{
  using namespace test;
  using rs::core::TrackColdHeader;
  using rs::core::TrackHotHeader;

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
    CHECK(sizeof(TrackColdHeader) == 48);
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

    // 2-byte section
    CHECK(offsetof(TrackColdHeader, trackNumber) == 32);
    CHECK(offsetof(TrackColdHeader, totalTracks) == 34);
    CHECK(offsetof(TrackColdHeader, discNumber) == 36);
    CHECK(offsetof(TrackColdHeader, totalDiscs) == 38);
    CHECK(offsetof(TrackColdHeader, customCount) == 40);
    CHECK(offsetof(TrackColdHeader, uriOffset) == 42);
    CHECK(offsetof(TrackColdHeader, uriLen) == 44);

    // 1-byte section
    CHECK(offsetof(TrackColdHeader, channels) == 46);
  }

} // anonymous namespace
