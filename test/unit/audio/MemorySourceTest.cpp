// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "ScriptedDecoderSession.h"
#include <ao/Error.h>
#include <ao/audio/DecoderTypes.h>
#include <ao/audio/Format.h>
#include <ao/audio/MemorySource.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <expected>
#include <memory>
#include <utility>
#include <vector>

namespace ao::audio::test
{
  TEST_CASE("MemorySource - Core Logic", "[audio][unit][memory_source]")
  {
    auto const format =
      Format{.sampleRate = 1000, .channels = 1, .bitDepth = 16, .isFloat = false, .isInterleaved = true};
    auto const info = DecodedStreamInfo{.sourceFormat = format, .outputFormat = format, .durationMs = 10};

    SECTION("Initialize buffers all blocks until EOS and closes decoder")
    {
      auto decoderPtr = std::make_unique<ScriptedDecoderSession>(info);
      auto* decoderRaw = decoderPtr.get();

      auto block1 = std::vector{std::byte{1}, std::byte{2}};
      auto block2 = std::vector{std::byte{3}, std::byte{4}};
      decoderRaw->setReadScript({{block1, false}, {block2, false}, {{}, true}});

      auto source = MemorySource{std::move(decoderPtr), info};
      REQUIRE(source.initialize());
      REQUIRE(decoderRaw->isClosed());
      REQUIRE(source.bufferedMs() == 2); // 4 bytes / (1000 Hz * 2 bytes/frame) = 2 ms
    }

    SECTION("Initialize converts decoder failure to DecodeFailed")
    {
      auto decoderPtr = std::make_unique<ScriptedDecoderSession>(info);
      decoderPtr->setReadScript(
        {{{}, false, std::unexpected(Error{.code = Error::Code::DecodeFailed, .message = "failure"})}});

      auto source = MemorySource{std::move(decoderPtr), info};
      auto result = source.initialize();
      REQUIRE_FALSE(result);
      REQUIRE(result.error().code == Error::Code::DecodeFailed);
      REQUIRE(result.error().message == "failure");
    }

    SECTION("Read returns bytes sequentially until drained")
    {
      auto decoderPtr = std::make_unique<ScriptedDecoderSession>(info);
      auto allData = std::vector{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
      decoderPtr->setReadScript({{allData, false}, {{}, true}});

      auto source = MemorySource{std::move(decoderPtr), info};
      REQUIRE(source.initialize());

      REQUIRE_FALSE(source.isDrained());

      auto out1 = std::vector<std::byte>(2);
      REQUIRE(source.read(out1) == 2);
      REQUIRE(out1[0] == std::byte{1});
      REQUIRE(out1[1] == std::byte{2});

      REQUIRE(source.bufferedMs() == 1);

      auto out2 = std::vector<std::byte>(10);
      REQUIRE(source.read(out2) == 2);
      REQUIRE(out2[0] == std::byte{3});
      REQUIRE(out2[1] == std::byte{4});

      REQUIRE(source.isDrained());
      REQUIRE(source.bufferedMs() == 0);
    }

    SECTION("Seek aligns 24-bit stereo offsets and clamps")
    {
      auto const format24 =
        Format{.sampleRate = 1000, .channels = 2, .bitDepth = 24, .isFloat = false, .isInterleaved = true};
      auto const info24 = DecodedStreamInfo{.sourceFormat = format24, .outputFormat = format24, .durationMs = 100};

      auto decoderPtr = std::make_unique<ScriptedDecoderSession>(info24);
      auto data = std::vector(60, std::byte{0}); // 10 frames of 6 bytes each = 10ms
      decoderPtr->setReadScript({{data, false}, {{}, true}});

      auto source = MemorySource{std::move(decoderPtr), info24};
      REQUIRE(source.initialize());

      // Seek to 1 ms = offset 6
      REQUIRE(source.seek(1));
      auto out = std::vector<std::byte>(6);
      REQUIRE(source.read(out) == 6);

      // Seek beyond end
      REQUIRE(source.seek(500));
      REQUIRE(source.isDrained());
    }

    SECTION("Zeroed output format yields zero buffered duration and zero seek offset")
    {
      auto const zeroFormat = Format{.sampleRate = 0, .channels = 0, .bitDepth = 0};
      auto const zeroInfo = DecodedStreamInfo{.sourceFormat = zeroFormat, .outputFormat = zeroFormat, .durationMs = 0};

      auto decoderPtr = std::make_unique<ScriptedDecoderSession>(zeroInfo);
      decoderPtr->setReadScript({{{}, true}});

      auto source = MemorySource{std::move(decoderPtr), zeroInfo};
      REQUIRE(source.initialize());

      REQUIRE(source.bufferedMs() == 0);
      REQUIRE(source.seek(100));
      REQUIRE(source.isDrained());
    }
  }
} // namespace ao::audio::test
