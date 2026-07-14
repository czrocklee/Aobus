// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/detail/StreamingBufferPolicy.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace ao::audio::detail::test
{
  TEST_CASE("StreamingBufferPolicy - duration target is rounded up and bounded by capacity",
            "[audio][unit][streaming-source]")
  {
    constexpr std::size_t kCapacity = 2097152;

    CHECK(bufferByteCountForDuration(176400, std::chrono::milliseconds{1}, kCapacity) == 177);
    CHECK(bufferByteCountForDuration(384000, std::chrono::milliseconds{1500}, kCapacity) == 576000);
    CHECK(bufferByteCountForDuration(1536000, std::chrono::milliseconds{1500}, kCapacity) == kCapacity);
    CHECK(bufferByteCountForDuration(1536000, std::chrono::milliseconds{500}, kCapacity) == 768000);
    CHECK(bufferByteCountForDuration(0, std::chrono::milliseconds{1500}, kCapacity) == 0);
    CHECK(bufferByteCountForDuration(1536000, std::chrono::milliseconds{0}, kCapacity) == 0);
    CHECK(bufferByteCountForDuration(1536000, std::chrono::milliseconds{-1}, kCapacity) == 0);
    CHECK(bufferByteCountForDuration(1536000, std::chrono::milliseconds{1500}, 0) == 0);
    CHECK(bufferByteCountForDuration(
            std::numeric_limits<std::uint64_t>::max(), std::chrono::milliseconds::max(), kCapacity) == kCapacity);
  }

  TEST_CASE("StreamingBufferPolicy - decode requires target space and previous-block headroom",
            "[audio][unit][streaming-source]")
  {
    constexpr std::size_t kCapacity = 2097152;
    constexpr std::size_t kBlockByteCount = 32768;

    CHECK(permitsDecode(kCapacity, kCapacity - kBlockByteCount, kBlockByteCount, kBlockByteCount));
    CHECK_FALSE(permitsDecode(kCapacity, kCapacity - kBlockByteCount + 1, kBlockByteCount - 1, kBlockByteCount));
    CHECK_FALSE(permitsDecode(kCapacity, kCapacity, 0, kBlockByteCount));

    constexpr std::size_t kNormalTarget = 576000;
    CHECK(permitsDecode(kNormalTarget, kNormalTarget - 1, kCapacity - kNormalTarget + 1, kBlockByteCount));
    CHECK_FALSE(permitsDecode(kNormalTarget, kNormalTarget, kCapacity - kNormalTarget, kBlockByteCount));
    CHECK(permitsDecode(kNormalTarget, 0, kCapacity, 0));
  }
} // namespace ao::audio::detail::test
