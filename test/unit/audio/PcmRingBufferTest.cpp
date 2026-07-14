// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/audio/PcmRingBuffer.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <vector>

namespace ao::audio::test
{
  namespace
  {
    constexpr auto kConcurrentCompletionTimeout = std::chrono::seconds{5};

    struct ByteMismatch final
    {
      std::int32_t index{};
      std::byte expected{};
      std::byte actual{};
    };
  } // namespace

  TEST_CASE("PcmRingBuffer - reads preserve FIFO bytes and clear resets state", "[audio][unit][ring-buffer]")
  {
    auto buffer = PcmRingBuffer{};

    SECTION("Empty read and write are no-ops")
    {
      CHECK(buffer.availableToWrite() == buffer.capacity());
      CHECK(buffer.write({}) == 0);
      CHECK(buffer.read({}) == 0);
      CHECK(buffer.size() == 0);
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
  }

  TEST_CASE("PcmRingBuffer - capacity limit causes short write instead of overflow", "[audio][unit][ring-buffer]")
  {
    auto buffer = PcmRingBuffer{};
    std::size_t const cap = buffer.capacity();

    SECTION("Capacity limit causes short write instead of overflow")
    {
      // Note: boost::lockfree::spsc_queue capacity is fixed at compile time.
      // PcmRingBuffer uses kRingBufferCapacity.
      auto largeData = std::vector(cap + 64, std::byte{0xCC});
      auto written = buffer.write(largeData);

      CHECK(written <= cap);
      CHECK(buffer.size() == written);

      // Fill remaining if any
      auto b = std::byte{0xDD};
      bool reachedCapacity = false;

      for (std::size_t attempts = 0; attempts <= cap; ++attempts)
      {
        auto const extraWritten = buffer.write(std::span<std::byte const>{&b, 1});

        if (extraWritten == 0)
        {
          reachedCapacity = true;
          break;
        }

        CHECK(extraWritten == 1);
      }

      CHECK(reachedCapacity);
      CHECK(buffer.write(std::span<std::byte const>{&b, 1}) == 0);
    }
  }

  TEST_CASE("PcmRingBuffer - single producer and consumer preserve byte order", "[audio][unit][ring-buffer]")
  {
    auto buffer = PcmRingBuffer{};
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
} // namespace ao::audio::test
