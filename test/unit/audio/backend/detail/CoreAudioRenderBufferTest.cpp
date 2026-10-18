// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/backend/detail/CoreAudioRenderBuffer.h"

#include <CoreAudioTypes/CoreAudioBaseTypes.h>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace ao::audio::backend::detail::test
{
  TEST_CASE("CoreAudioRenderBuffer - supplies preallocated storage for a null AUHAL buffer", "[audio][unit][coreaudio]")
  {
    auto staging = std::array<std::byte, 32>{};
    auto buffers = ::AudioBufferList{
      .mNumberBuffers = 1U, .mBuffers = {{.mNumberChannels = 2U, .mDataByteSize = 16U, .mData = nullptr}}};

    std::uint32_t clientChannels = 2U;

    SECTION("matching mono channels")
    {
      clientChannels = 1U;
      buffers.mBuffers[0].mNumberChannels = 1U;
    }

    SECTION("matching stereo channels")
    {
    }

    auto const bound = bindCoreAudioRenderBuffer(&buffers, staging, 16U, clientChannels);

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

    std::uint32_t clientChannels = 2U;

    SECTION("matching mono channels")
    {
      clientChannels = 1U;
      buffers.mBuffers[0].mNumberChannels = 1U;
    }

    SECTION("matching stereo channels")
    {
    }

    auto const bound = bindCoreAudioRenderBuffer(&buffers, staging, 12U, clientChannels);
    REQUIRE(bound.valid);
    CHECK(bound.output.data() == native.data());
    CHECK(bound.output.size() == 12U);
    CHECK(buffers.mBuffers[0].mDataByteSize == 12U);

    buffers.mBuffers[0].mDataByteSize = 8U;
    CHECK_FALSE(bindCoreAudioRenderBuffer(&buffers, staging, 12U, clientChannels).valid);
    buffers.mNumberBuffers = 0U;
    CHECK_FALSE(bindCoreAudioRenderBuffer(&buffers, staging, 8U, clientChannels).valid);
    CHECK_FALSE(bindCoreAudioRenderBuffer(nullptr, staging, 8U, clientChannels).valid);
  }

  TEST_CASE("CoreAudioRenderBuffer - rejects channel mismatches before mutating provided or null binding",
            "[audio][unit][coreaudio]")
  {
    auto staging = std::array<std::byte, 32>{};
    auto native = std::array<std::byte, 32>{};
    auto buffers = ::AudioBufferList{
      .mNumberBuffers = 1U, .mBuffers = {{.mNumberChannels = 1U, .mDataByteSize = 24U, .mData = native.data()}}};
    std::uint32_t clientChannels = 2U;

    SECTION("wrong native channel count with adequate provided storage")
    {
    }

    SECTION("wrong native channel count with null storage")
    {
      buffers.mBuffers[0].mData = nullptr;
    }

    SECTION("zero client and native counts with adequate provided storage")
    {
      clientChannels = 0U;
      buffers.mBuffers[0].mNumberChannels = 0U;
    }

    SECTION("zero client and native counts with null storage")
    {
      clientChannels = 0U;
      buffers.mBuffers[0].mNumberChannels = 0U;
      buffers.mBuffers[0].mData = nullptr;
    }

    SECTION("zero native count cannot carry a nonzero client")
    {
      buffers.mBuffers[0].mNumberChannels = 0U;
    }

    auto const original = buffers.mBuffers[0];
    auto const bound = bindCoreAudioRenderBuffer(&buffers, staging, 12U, clientChannels);
    CHECK_FALSE(bound.valid);
    CHECK(bound.output.empty());
    CHECK(buffers.mNumberBuffers == 1U);
    CHECK(buffers.mBuffers[0].mData == original.mData);
    CHECK(buffers.mBuffers[0].mDataByteSize == 24U);
    CHECK(buffers.mBuffers[0].mNumberChannels == original.mNumberChannels);
  }
} // namespace ao::audio::backend::detail::test
