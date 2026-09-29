// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "lib/audio/PcmRingBuffer.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <semaphore>
#include <span>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

namespace ao::audio::test
{
  namespace
  {
    constexpr auto kConcurrentCompletionTimeout = std::chrono::seconds{5};

    // 8-bit mono PCM: a one-byte frame keeps the byte-FIFO mechanics directly
    // observable in the mechanics cases below.
    constexpr std::size_t kSingleByteFrameSize = 1;

    struct ByteMismatch final
    {
      std::int32_t index{};
      std::byte expected{};
      std::byte actual{};
    };

    // Producer outcome recorded when the concurrent frame case aborts at its
    // deadline or a stop request instead of committing every frame.
    struct ProducerStall final
    {
      std::size_t committedFrames{};
      std::size_t parkedBytes{};
    };
  } // namespace

  TEST_CASE("PcmRingBuffer - reads preserve FIFO bytes and clear resets state", "[audio][unit][ring-buffer]")
  {
    auto buffer = PcmRingBuffer{kSingleByteFrameSize};

    SECTION("Empty read and write are no-ops")
    {
      CHECK(buffer.availableToWrite() == buffer.capacity());
      CHECK(buffer.write({}) == 0);
      CHECK(buffer.read({}) == 0);
      buffer.clear();
      CHECK(buffer.size() == 0);
      CHECK(buffer.availableToWrite() == buffer.capacity());
    }

    SECTION("Preserves FIFO order across multiple writes and reads")
    {
      auto dataA = std::vector{std::byte{1}, std::byte{2}, std::byte{3}};
      auto dataB = std::vector{std::byte{4}, std::byte{5}};

      CHECK(buffer.write(dataA) == 3);
      CHECK(buffer.write(dataB) == 2);
      CHECK(buffer.size() == 5);
      CHECK(buffer.availableToWrite() == buffer.capacity() - 5);

      auto output = std::vector<std::byte>(5);
      REQUIRE(buffer.read(std::span{output}.subspan(0, 2)) == 2);
      CHECK(output[0] == std::byte{1});
      CHECK(output[1] == std::byte{2});
      CHECK(buffer.size() == 3);
      CHECK(buffer.availableToWrite() == buffer.capacity() - 3);

      REQUIRE(buffer.read(std::span{output}.subspan(2, 3)) == 3);
      CHECK(output[2] == std::byte{3});
      CHECK(output[3] == std::byte{4});
      CHECK(output[4] == std::byte{5});
      CHECK(buffer.size() == 0);
    }

    SECTION("Partial read leaves remaining bytes available")
    {
      auto data = std::vector(10, std::byte{0xAA});
      buffer.write(data);

      auto output = std::vector<std::byte>(4);
      REQUIRE(buffer.read(output) == 4);
      REQUIRE(buffer.size() == 6);

      auto outputRemaining = std::vector<std::byte>(10);
      REQUIRE(buffer.read(outputRemaining) == 6);
      REQUIRE(buffer.size() == 0);
    }

    SECTION("Clear resets size and supports reuse")
    {
      auto data = std::vector(10, std::byte{0xBB});
      buffer.write(data);
      REQUIRE(buffer.size() == 10);

      buffer.clear();
      CHECK(buffer.size() == 0);

      auto output = std::vector<std::byte>(10);
      CHECK(buffer.read(output) == 0);

      buffer.write(data);
      CHECK(buffer.size() == 10);
      REQUIRE(buffer.read(output) == 10);
      CHECK(output[0] == std::byte{0xBB});
    }

    SECTION("Clear resets wrapped state and preserves FIFO order after reuse")
    {
      auto const capacity = buffer.capacity();
      auto initial = std::vector(capacity, std::byte{0x11});
      REQUIRE(buffer.write(initial) == capacity);

      auto discarded = std::vector<std::byte>(capacity / 2);
      REQUIRE(buffer.read(discarded) == discarded.size());

      auto wrapped = std::vector(capacity / 2, std::byte{0x22});
      REQUIRE(buffer.write(wrapped) == wrapped.size());
      REQUIRE(buffer.size() == capacity);

      buffer.clear();
      CHECK(buffer.size() == 0);
      CHECK(buffer.availableToWrite() == capacity);

      auto replacement = std::vector{std::byte{1}, std::byte{2}, std::byte{3}};
      REQUIRE(buffer.write(replacement) == replacement.size());

      auto output = std::vector<std::byte>(replacement.size());
      REQUIRE(buffer.read(output) == output.size());
      CHECK(output == replacement);
    }
  }

  TEST_CASE("PcmRingBuffer - capacity limit causes short write instead of overflow", "[audio][unit][ring-buffer]")
  {
    auto buffer = PcmRingBuffer{kSingleByteFrameSize};
    auto const capacity = buffer.capacity();
    REQUIRE(capacity > 0);
    auto const largeData = std::vector(capacity + 64, std::byte{0xCC});

    REQUIRE(buffer.write(largeData) == capacity);
    CHECK(buffer.size() == capacity);
    CHECK(buffer.availableToWrite() == 0);

    auto extra = std::byte{0xDD};
    CHECK(buffer.write(std::span{&extra, 1}) == 0);
    CHECK(buffer.size() == capacity);

    auto output = std::vector(capacity + 64, std::byte{0xEE});
    REQUIRE(buffer.read(output) == capacity);
    CHECK(
      std::ranges::all_of(std::span{output}.first(capacity), [](std::byte value) { return value == std::byte{0xCC}; }));
    CHECK(std::ranges::all_of(
      std::span{output}.subspan(capacity), [](std::byte value) { return value == std::byte{0xEE}; }));
    CHECK(buffer.size() == 0);
    CHECK(buffer.availableToWrite() == capacity);

    REQUIRE(buffer.write(std::span{&extra, 1}) == 1);
    auto recovered = std::byte{};
    REQUIRE(buffer.read(std::span{&recovered, 1}) == 1);
    CHECK(recovered == extra);
  }

  TEST_CASE("PcmRingBuffer - frame-aligned ring parks partial frames at frame boundaries", "[audio][unit][ring-buffer]")
  {
    // 24-bit stereo frames (6 bytes) do not divide the 2 MiB capacity, so
    // space pressure can only resolve at whole-frame boundaries.
    constexpr std::size_t kFrameSize = 6;
    auto const usableCapacity = (kRingBufferCapacity / kFrameSize) * kFrameSize;
    auto const fillBytes = (usableCapacity / kFrameSize / 2) * kFrameSize;

    SECTION("capacity and writable space floor to whole frames")
    {
      auto buffer = PcmRingBuffer{kFrameSize};
      CHECK(buffer.frameSize() == kFrameSize);
      CHECK(buffer.capacity() == usableCapacity);
      CHECK(buffer.capacity() % kFrameSize == 0);
      CHECK(buffer.availableToWrite() == usableCapacity);

      auto const block = std::vector(kRingBufferCapacity, std::byte{0x33});
      CHECK(buffer.write(block) == usableCapacity);
      CHECK(buffer.size() == usableCapacity);
      CHECK(buffer.availableToWrite() == 0);
    }

    SECTION("truncated write parks whole frames and reads lose no bytes")
    {
      auto buffer = PcmRingBuffer{kFrameSize};
      auto const fill = std::vector(fillBytes, std::byte{0x11});
      REQUIRE(buffer.write(fill) == fillBytes);

      // Larger than the remaining free space: only whole frames are accepted
      // and the remainder stays parked with the producer.
      auto const blockBytes = ((usableCapacity - fillBytes) + (kFrameSize * 2));
      auto const block = std::vector(blockBytes, std::byte{0x22});
      auto const written = buffer.write(block);
      CHECK(written == usableCapacity - fillBytes);
      CHECK(written % kFrameSize == 0);
      CHECK(buffer.size() == usableCapacity);
      CHECK(buffer.availableToWrite() == 0);

      // Partial-frame read requests return nothing; larger requests floor to
      // whole frames and consume only previously accepted bytes.
      auto partial = std::array<std::byte, 32>{};
      CHECK(buffer.read(std::span{partial}.first(4)) == 0);
      CHECK(buffer.read(std::span{partial}.first(7)) == kFrameSize);
      CHECK(buffer.read(std::span{partial}.first(13)) == kFrameSize * 2);
      CHECK(std::ranges::all_of(
        std::span{partial}.first(kFrameSize * 2), [](std::byte value) { return value == std::byte{0x11}; }));
      CHECK(buffer.size() == usableCapacity - (kFrameSize * 3));

      // Freed space admits the parked frames; they become visible in order.
      REQUIRE(buffer.write(std::span{block}.subspan(written)) == kFrameSize * 2);
      CHECK(buffer.size() == usableCapacity - kFrameSize);

      // Draining yields the exact FIFO stream: no bytes lost, none duplicated.
      auto drained = std::vector<std::byte>(usableCapacity);
      REQUIRE(buffer.read(drained) == usableCapacity - kFrameSize);
      CHECK(buffer.size() == 0);
      CHECK(buffer.availableToWrite() == usableCapacity);
      CHECK((kFrameSize * 3) + (usableCapacity - kFrameSize) == fillBytes + blockBytes);
      CHECK(std::ranges::all_of(std::span{drained}.first(fillBytes - (kFrameSize * 3)),
                                [](std::byte value) { return value == std::byte{0x11}; }));
      CHECK(std::ranges::all_of(std::span{drained}.subspan(fillBytes - (kFrameSize * 3), blockBytes),
                                [](std::byte value) { return value == std::byte{0x22}; }));
    }
  }

  TEST_CASE("PcmRingBuffer - single producer and consumer preserve byte order",
            "[audio][unit][ring-buffer][concurrency][stress]")
  {
    auto buffer = PcmRingBuffer{kSingleByteFrameSize};
    std::int32_t const iterations = 10000;

    auto progressMutex = std::mutex{};
    auto progressChanged = std::condition_variable{};
    auto const deadline = std::chrono::steady_clock::now() + kConcurrentCompletionTimeout;

    std::int32_t produced = 0;
    std::int32_t consumed = 0;
    bool producerDone = false;
    auto optProducerWriteTimeoutAt = std::optional<std::int32_t>{};
    auto optConsumerTimeoutAt = std::optional<std::int32_t>{};
    auto optConsumerReadFailureAt = std::optional<std::int32_t>{};
    auto optMismatch = std::optional<ByteMismatch>{};

    auto producer = std::jthread{[&]
                                 {
                                   for (std::int32_t i = 0; i < iterations; ++i)
                                   {
                                     std::byte b = static_cast<std::byte>(i % 256);

                                     while (buffer.write(std::span{&b, 1}) == 0)
                                     {
                                       auto lock = std::unique_lock{progressMutex};

                                       if (auto const observedConsumed = consumed; !progressChanged.wait_until(
                                             lock, deadline, [&] { return consumed > observedConsumed; }))
                                       {
                                         optProducerWriteTimeoutAt = i;
                                         producerDone = true;
                                         progressChanged.notify_all();
                                         return;
                                       }
                                     }

                                     {
                                       auto lock = std::scoped_lock{progressMutex};
                                       ++produced;
                                     }

                                     progressChanged.notify_all();
                                   }

                                   {
                                     auto lock = std::scoped_lock{progressMutex};
                                     producerDone = true;
                                   }

                                   progressChanged.notify_all();
                                 }};

    auto consumer = std::jthread{
      [&]
      {
        std::int32_t count = 0;

        while (count < iterations)
        {
          {
            auto lock = std::unique_lock{progressMutex};

            if (!progressChanged.wait_until(lock, deadline, [&] { return produced > count || producerDone; }))
            {
              optConsumerTimeoutAt = count;
              return;
            }

            if (produced <= count)
            {
              return;
            }
          }

          auto b = std::byte{};

          if (buffer.read(std::span{&b, 1}) != 1)
          {
            auto lock = std::scoped_lock{progressMutex};
            optConsumerReadFailureAt = count;
            return;
          }

          if (auto const expected = static_cast<std::byte>(count % 256); b != expected)
          {
            auto lock = std::scoped_lock{progressMutex};
            optMismatch = ByteMismatch{.index = count, .expected = expected, .actual = b};
            return;
          }

          ++count;
          {
            auto lock = std::scoped_lock{progressMutex};
            consumed = count;
          }
          progressChanged.notify_all();
        }
      }};

    producer.join();
    consumer.join();

    if (optProducerWriteTimeoutAt)
    {
      FAIL("Producer timed out waiting for capacity at iteration " << *optProducerWriteTimeoutAt);
    }

    if (optConsumerTimeoutAt)
    {
      FAIL("Consumer timed out after reading " << *optConsumerTimeoutAt << " of " << iterations << " bytes");
    }

    if (optConsumerReadFailureAt)
    {
      FAIL("Consumer observed a produced byte but read no data at iteration " << *optConsumerReadFailureAt);
    }

    if (optMismatch)
    {
      FAIL("Byte mismatch at iteration " << optMismatch->index << ": expected "
                                         << std::to_integer<int>(optMismatch->expected) << " but read "
                                         << std::to_integer<int>(optMismatch->actual));
    }

    CHECK(consumed == iterations);
    CHECK(buffer.size() == 0);
  }

  TEST_CASE("PcmRingBuffer - concurrent frame-aligned producer and consumer park whole frames",
            "[audio][unit][ring-buffer][concurrency][stress]")
  {
    // 24-bit stereo frames do not divide the capacity; the producer total
    // exceeds the whole-frame capacity, so a parked partial write across
    // threads is guaranteed while the consumer is held back.
    constexpr std::size_t kFrameSize = 6;
    constexpr std::size_t kTotalFrames = 400000;
    constexpr std::size_t kProducerBurstFrames = 1024;
    constexpr std::size_t kConsumerBurstFrames = 97;
    constexpr std::size_t kNoMismatchSentinel = kTotalFrames;

    auto buffer = PcmRingBuffer{kFrameSize};
    auto writeMisaligned = std::atomic<std::size_t>{0};
    auto readMisaligned = std::atomic<std::size_t>{0};
    auto orderMismatchAt = std::atomic<std::size_t>{kNoMismatchSentinel};
    auto producerParked = std::atomic<bool>{false};
    auto producerStart = std::binary_semaphore{0};
    auto optProducerStall = std::optional<ProducerStall>{};
    auto optConsumerTimeoutAt = std::optional<std::size_t>{};
    std::size_t consumedFrames = 0;
    auto const deadline = std::chrono::steady_clock::now() + kConcurrentCompletionTimeout;

    auto const frameValue = [](std::size_t frameIndex) { return static_cast<std::byte>((frameIndex % 251) + 1); };

    auto producer = std::jthread{
      [&](std::stop_token const& stopToken)
      {
        auto burst = std::vector<std::byte>(kProducerBurstFrames * kFrameSize);
        std::size_t producedFrames = 0;

        while (producedFrames < kTotalFrames)
        {
          if (std::chrono::steady_clock::now() >= deadline || stopToken.stop_requested())
          {
            optProducerStall = ProducerStall{.committedFrames = producedFrames, .parkedBytes = 0};
            break;
          }

          auto const burstFrames = std::min(kProducerBurstFrames, kTotalFrames - producedFrames);

          for (std::size_t frame = 0; frame < burstFrames; ++frame)
          {
            std::ranges::fill(
              std::span{burst}.subspan(frame * kFrameSize, kFrameSize), frameValue(producedFrames + frame));
          }

          auto pending = std::span<std::byte const>{burst}.first(burstFrames * kFrameSize);

          while (!pending.empty())
          {
            auto const written = buffer.write(pending);

            if (written % kFrameSize != 0)
            {
              writeMisaligned.fetch_add(1, std::memory_order_relaxed);
            }

            pending = pending.subspan(written);

            if (pending.empty())
            {
              break;
            }

            // One-shot startup signal: only the first park releases, so
            // park retries never accumulate past the binary maximum.
            if (!producerParked.exchange(true, std::memory_order_relaxed))
            {
              producerStart.release();
            }

            // Only the consumer can clear the parked tail, so leaving the
            // park requires the deadline or the stop request of unwinding.
            if (std::chrono::steady_clock::now() >= deadline || stopToken.stop_requested())
            {
              auto const committedFrames = producedFrames + burstFrames - (pending.size() / kFrameSize);
              optProducerStall = ProducerStall{.committedFrames = committedFrames, .parkedBytes = pending.size()};
              break;
            }

            std::this_thread::yield();
          }

          if (optProducerStall)
          {
            break;
          }

          producedFrames += burstFrames;
        }

        // Exit fallback keeps the startup signal one-shot: it fires only
        // when no park released it first, whatever ended the run.
        if (!producerParked.load(std::memory_order_relaxed))
        {
          producerStart.release();
        }
      }};

    REQUIRE(producerStart.try_acquire_for(kConcurrentCompletionTimeout));

    auto consumer =
      std::jthread{[&]
                   {
                     auto output = std::vector<std::byte>(kConsumerBurstFrames * kFrameSize);

                     while (consumedFrames < kTotalFrames && std::chrono::steady_clock::now() < deadline)
                     {
                       auto const bytesRead = buffer.read(output);

                       if (bytesRead % kFrameSize != 0)
                       {
                         readMisaligned.fetch_add(1, std::memory_order_relaxed);
                       }

                       for (std::size_t frame = 0; frame < bytesRead / kFrameSize; ++frame)
                       {
                         auto const expected = frameValue(consumedFrames + frame);
                         auto const actual = std::span{output}.subspan(frame * kFrameSize, kFrameSize);

                         if (!std::ranges::all_of(actual, [&](std::byte value) { return value == expected; }))
                         {
                           auto sentinel = kNoMismatchSentinel;
                           std::ignore = orderMismatchAt.compare_exchange_strong(sentinel, consumedFrames + frame);
                         }
                       }

                       consumedFrames += bytesRead / kFrameSize;

                       if (bytesRead == 0)
                       {
                         std::this_thread::sleep_for(std::chrono::microseconds{100});
                       }
                     }

                     if (consumedFrames < kTotalFrames)
                     {
                       optConsumerTimeoutAt = consumedFrames;
                     }
                   }};

    producer.join();
    consumer.join();

    if (optProducerStall)
    {
      FAIL("Producer stalled with " << optProducerStall->committedFrames << " of " << kTotalFrames
                                    << " frames committed and " << optProducerStall->parkedBytes
                                    << " bytes parked at the deadline or stop request");
    }

    if (optConsumerTimeoutAt)
    {
      FAIL("Consumer timed out after " << *optConsumerTimeoutAt << " of " << kTotalFrames << " frames");
    }

    REQUIRE(producerParked.load());
    CHECK(writeMisaligned.load() == 0);
    CHECK(readMisaligned.load() == 0);
    CHECK(orderMismatchAt.load() == kNoMismatchSentinel);
    CHECK(consumedFrames == kTotalFrames);
    CHECK(buffer.size() == 0);
  }
} // namespace ao::audio::test
