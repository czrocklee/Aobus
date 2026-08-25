// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/backend/detail/AudioBackendRenderBuffer.h"

#include <ao/audio/RenderTarget.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ao::audio::backend::detail::test
{
  TEST_CASE("prepareAudioBackendRenderBuffer zero-fills a short underrun and advances committed position",
            "[audio][unit][render-buffer]")
  {
    auto buffer = std::array<std::byte, 16>{};
    buffer.fill(std::byte{0x7F});

    auto const prepared = prepareAudioBackendRenderBuffer(
      buffer,
      4,
      {.bytesWritten = 8, .positionFrameOffset = 1, .positionFrames = 4, .drained = false});

    CHECK(prepared.renderedFrames == 2);
    CHECK(prepared.framesProvided == 4);
    CHECK(prepared.positionFrames == 1);
    CHECK(prepared.underrun);
    CHECK_FALSE(prepared.drained);
    CHECK(buffer[7] == std::byte{0x7F});
    CHECK(buffer[8] == std::byte{0});
    CHECK(buffer[15] == std::byte{0});
  }

  TEST_CASE("prepareAudioBackendRenderBuffer silences a drained suffix without reporting underrun",
            "[audio][unit][render-buffer]")
  {
    auto buffer = std::array<std::byte, 12>{};
    buffer.fill(std::byte{0x3A});

    auto const prepared = prepareAudioBackendRenderBuffer(
      buffer,
      4,
      {.bytesWritten = 4, .positionFrameOffset = 0, .positionFrames = 1, .drained = true});

    CHECK(prepared.renderedFrames == 1);
    CHECK(prepared.framesProvided == 3);
    CHECK(prepared.positionFrames == 1);
    CHECK_FALSE(prepared.underrun);
    CHECK(prepared.drained);
    CHECK(buffer[3] == std::byte{0x3A});
    CHECK(buffer[4] == std::byte{0});
    CHECK(buffer[11] == std::byte{0});
  }

  TEST_CASE("prepareAudioBackendRenderBuffer clips an overreported result to the native buffer",
            "[audio][unit][render-buffer]")
  {
    auto buffer = std::array<std::byte, 8>{};

    auto const prepared = prepareAudioBackendRenderBuffer(
      buffer,
      4,
      {.bytesWritten = 40, .positionFrameOffset = 0, .positionFrames = 9, .drained = false});

    CHECK(prepared.renderedFrames == 2);
    CHECK(prepared.framesProvided == 2);
    CHECK(prepared.positionFrames == 2);
    CHECK_FALSE(prepared.underrun);
  }

  TEST_CASE("prepareAudioBackendRenderBuffer rejects a zero frame size without touching storage",
            "[audio][unit][render-buffer]")
  {
    auto buffer = std::array<std::byte, 4>{};
    buffer.fill(std::byte{0x55});

    auto const prepared = prepareAudioBackendRenderBuffer(buffer, 0, {.bytesWritten = 4, .drained = true});

    CHECK(prepared.renderedFrames == 0);
    CHECK(prepared.framesProvided == 0);
    CHECK(prepared.positionFrames == 0);
    CHECK_FALSE(prepared.underrun);
    CHECK(prepared.drained);
    CHECK(buffer.front() == std::byte{0x55});
    CHECK(buffer.back() == std::byte{0x55});
  }
} // namespace ao::audio::backend::detail::test
