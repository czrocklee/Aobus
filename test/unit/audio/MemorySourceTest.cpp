// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "ScriptedDecoderSession.h"
#include <ao/audio/MemorySource.h>
#include <catch2/catch_test_macros.hpp>

using namespace ao::audio;

TEST_CASE("MemorySource - Core Logic", "[audio][unit][memory_source]")
{
  Format format{.sampleRate = 1000, .channels = 1, .bitDepth = 16, .isFloat = false, .isInterleaved = true};
  DecodedStreamInfo info{.sourceFormat = format, .outputFormat = format, .durationMs = 10};

  SECTION("Initialize buffers all blocks until EOS and closes decoder")
  {
    auto decoder = std::make_unique<ScriptedDecoderSession>(info);
    auto* decoderPtr = decoder.get();

    std::vector<std::byte> block1 = {std::byte{1}, std::byte{2}};
    std::vector<std::byte> block2 = {std::byte{3}, std::byte{4}};
    decoderPtr->setReadScript({{block1, false}, {block2, false}, {{}, true}});

    MemorySource source(std::move(decoder), info);
    REQUIRE(source.initialize());
    REQUIRE(decoderPtr->isClosed());
    REQUIRE(source.bufferedMs() == 2); // 4 bytes / (1000 Hz * 2 bytes/frame) = 2 ms
  }

  SECTION("Initialize converts decoder failure to DecodeFailed")
  {
    auto decoder = std::make_unique<ScriptedDecoderSession>(info);
    decoder->setReadScript(
      {{{}, false, std::unexpected(ao::Error{.code = ao::Error::Code::DecodeFailed, .message = "failure"})}});

    MemorySource source(std::move(decoder), info);
    auto result = source.initialize();
    REQUIRE_FALSE(result);
    REQUIRE(result.error().code == ao::Error::Code::DecodeFailed);
    REQUIRE(result.error().message == "failure");
  }

  SECTION("Read returns bytes sequentially until drained")
  {
    auto decoder = std::make_unique<ScriptedDecoderSession>(info);
    std::vector<std::byte> allData = {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    decoder->setReadScript({{allData, false}, {{}, true}});

    MemorySource source(std::move(decoder), info);
    source.initialize();

    REQUIRE_FALSE(source.isDrained());

    std::vector<std::byte> out1(2);
    REQUIRE(source.read(out1) == 2);
    REQUIRE(out1[0] == std::byte{1});
    REQUIRE(out1[1] == std::byte{2});

    REQUIRE(source.bufferedMs() == 1);

    std::vector<std::byte> out2(10);
    REQUIRE(source.read(out2) == 2);
    REQUIRE(out2[0] == std::byte{3});
    REQUIRE(out2[1] == std::byte{4});

    REQUIRE(source.isDrained());
    REQUIRE(source.bufferedMs() == 0);
  }

  SECTION("Seek aligns 24-bit stereo offsets and clamps")
  {
    Format format24{.sampleRate = 1000, .channels = 2, .bitDepth = 24, .isFloat = false, .isInterleaved = true};
    DecodedStreamInfo info24{.sourceFormat = format24, .outputFormat = format24, .durationMs = 100};

    auto decoder = std::make_unique<ScriptedDecoderSession>(info24);
    std::vector<std::byte> data(60, std::byte{0}); // 10 frames of 6 bytes each = 10ms
    decoder->setReadScript({{data, false}, {{}, true}});

    MemorySource source(std::move(decoder), info24);
    source.initialize();

    // Seek to 1 ms = offset 6
    REQUIRE(source.seek(1));
    std::vector<std::byte> out(6);
    REQUIRE(source.read(out) == 6);

    // Seek beyond end
    REQUIRE(source.seek(500));
    REQUIRE(source.isDrained());
  }

  SECTION("Zeroed output format yields zero buffered duration and zero seek offset")
  {
    Format zeroFormat{.sampleRate = 0, .channels = 0, .bitDepth = 0};
    DecodedStreamInfo zeroInfo{.sourceFormat = zeroFormat, .outputFormat = zeroFormat, .durationMs = 0};

    auto decoder = std::make_unique<ScriptedDecoderSession>(zeroInfo);
    decoder->setReadScript({{{}, true}});

    MemorySource source(std::move(decoder), zeroInfo);
    source.initialize();

    REQUIRE(source.bufferedMs() == 0);
    REQUIRE(source.seek(100));
    REQUIRE(source.isDrained());
  }
}
