// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/backend/detail/CoreAudioRenderBuffer.h"

#include <CoreAudioTypes/CoreAudioBaseTypes.h>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>

namespace ao::audio::backend::detail::test
{
  TEST_CASE("CoreAudioRenderBuffer - supplies preallocated storage for a null AUHAL buffer", "[audio][unit][coreaudio]")
  {
    auto staging = std::array<std::byte, 32>{};
    auto buffers = ::AudioBufferList{
      .mNumberBuffers = 1U, .mBuffers = {{.mNumberChannels = 2U, .mDataByteSize = 16U, .mData = nullptr}}};

    auto const bound = bindCoreAudioRenderBuffer(&buffers, staging, 16U);

    REQUIRE(bound.valid);
    CHECK(bound.output.data() == staging.data());
    CHECK(bound.output.size() == 16U);
    CHECK(buffers.mBuffers[0].mData == staging.data());
    CHECK(buffers.mBuffers[0].mDataByteSize == 16U);
  }

  TEST_CASE("CoreAudioRenderBuffer - preserves adequate native storage and rejects bad shapes",
            "[audio][unit][coreaudio]")
  {
    auto staging = std::array<std::byte, 32>{};
    auto native = std::array<std::byte, 16>{};
    auto buffers = ::AudioBufferList{
      .mNumberBuffers = 1U, .mBuffers = {{.mNumberChannels = 2U, .mDataByteSize = 16U, .mData = native.data()}}};

    auto const bound = bindCoreAudioRenderBuffer(&buffers, staging, 12U);
    REQUIRE(bound.valid);
    CHECK(bound.output.data() == native.data());
    CHECK(bound.output.size() == 12U);
    CHECK(buffers.mBuffers[0].mDataByteSize == 12U);

    buffers.mBuffers[0].mDataByteSize = 8U;
    CHECK_FALSE(bindCoreAudioRenderBuffer(&buffers, staging, 12U).valid);
    buffers.mNumberBuffers = 0U;
    CHECK_FALSE(bindCoreAudioRenderBuffer(&buffers, staging, 8U).valid);
    CHECK_FALSE(bindCoreAudioRenderBuffer(nullptr, staging, 8U).valid);
  }
} // namespace ao::audio::backend::detail::test
