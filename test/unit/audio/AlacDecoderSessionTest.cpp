// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ao/audio/AlacDecoderSession.h"

#include "ao/audio/DecoderTypes.h"
#include "ao/audio/Format.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>

namespace ao::audio::test
{
  TEST_CASE("AlacDecoderSession - Seek", "[audio][alac][seek]")
  {
    auto const testFile = std::filesystem::path{TAG_TEST_DATA_DIR} / "hires.m4a";

    if (!std::filesystem::exists(testFile))
    {
      SKIP("Test file 'hires.m4a' missing");
    }

    auto decoder = AlacDecoderSession{Format{.bitDepth = 24, .isInterleaved = true}};
    REQUIRE(decoder.open(testFile));

    auto const info = decoder.streamInfo();
    REQUIRE(info.sourceFormat.sampleRate > 0);
    REQUIRE(info.durationMs > 500);

    auto const firstBlock = decoder.readNextBlock();
    REQUIRE(firstBlock);
    CHECK(firstBlock->firstFrameIndex == 0);

    constexpr std::uint32_t kSeekPositionMs = 500;
    auto const targetFrame = (static_cast<std::uint64_t>(kSeekPositionMs) * info.sourceFormat.sampleRate) / 1000U;

    REQUIRE(decoder.seek(kSeekPositionMs));
    auto const soughtBlock = decoder.readNextBlock();

    REQUIRE(soughtBlock);
    REQUIRE(soughtBlock->frames > 0);
    CHECK(soughtBlock->firstFrameIndex > 0);
    CHECK(soughtBlock->firstFrameIndex <= targetFrame);
    CHECK(soughtBlock->firstFrameIndex + soughtBlock->frames > targetFrame);

    REQUIRE(decoder.seek(0));
    auto const resetBlock = decoder.readNextBlock();

    REQUIRE(resetBlock);
    CHECK(resetBlock->firstFrameIndex == 0);
  }
} // namespace ao::audio::test
