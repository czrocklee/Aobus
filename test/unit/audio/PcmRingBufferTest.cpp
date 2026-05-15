// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/audio/PcmRingBuffer.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <span>
#include <thread>
#include <vector>

namespace ao::audio::test
{
  TEST_CASE("PcmRingBuffer - Core Contract", "[audio][unit][ring_buffer]")
  {
    auto buffer = PcmRingBuffer{};

    SECTION("Empty read and write are no-ops")
    {
      REQUIRE(buffer.write({}) == 0);
      REQUIRE(buffer.read({}) == 0);
      REQUIRE(buffer.size() == 0);
    }

    SECTION("Preserves FIFO order across multiple writes and reads")
    {
      std::vector<std::byte> dataA = {std::byte{1}, std::byte{2}, std::byte{3}};
      std::vector<std::byte> dataB = {std::byte{4}, std::byte{5}};

      REQUIRE(buffer.write(dataA) == 3);
      REQUIRE(buffer.write(dataB) == 2);
      REQUIRE(buffer.size() == 5);

      std::vector<std::byte> output(5);
      REQUIRE(buffer.read(std::span(output).subspan(0, 2)) == 2);
      REQUIRE(output[0] == std::byte{1});
      REQUIRE(output[1] == std::byte{2});
      REQUIRE(buffer.size() == 3);

      REQUIRE(buffer.read(std::span(output).subspan(2, 3)) == 3);
      REQUIRE(output[2] == std::byte{3});
      REQUIRE(output[3] == std::byte{4});
      REQUIRE(output[4] == std::byte{5});
      REQUIRE(buffer.size() == 0);
    }

    SECTION("Partial read leaves remaining bytes available")
    {
      std::vector<std::byte> data(10, std::byte{0xAA});
      buffer.write(data);

      std::vector<std::byte> output(4);
      REQUIRE(buffer.read(output) == 4);
      REQUIRE(buffer.size() == 6);

      std::vector<std::byte> outputRemaining(10);
      REQUIRE(buffer.read(outputRemaining) == 6);
      REQUIRE(buffer.size() == 0);
    }

    SECTION("Clear resets size and supports reuse")
    {
      std::vector<std::byte> data(10, std::byte{0xBB});
      buffer.write(data);
      REQUIRE(buffer.size() == 10);

      buffer.clear();
      REQUIRE(buffer.size() == 0);

      std::vector<std::byte> output(10);
      REQUIRE(buffer.read(output) == 0);

      buffer.write(data);
      REQUIRE(buffer.size() == 10);
      REQUIRE(buffer.read(output) == 10);
      REQUIRE(output[0] == std::byte{0xBB});
    }
  }

  TEST_CASE("PcmRingBuffer - Capacity Limits", "[audio][unit][ring_buffer]")
  {
    auto buffer = PcmRingBuffer{};
    std::size_t const cap = buffer.capacity();

    SECTION("Capacity limit causes short write instead of overflow")
    {
      // Note: boost::lockfree::spsc_queue capacity is fixed at compile time.
      // PcmRingBuffer uses kRingBufferCapacity.
      std::vector<std::byte> largeData(cap + 64, std::byte{0xCC});
      auto written = buffer.write(largeData);

      REQUIRE(written <= cap);
      REQUIRE(buffer.size() == written);

      // Fill remaining if any
      auto b = std::byte{0xDD};

      while (buffer.write(std::span<std::byte const>(&b, 1)) == 1)
      {
      }

      REQUIRE(buffer.write(std::span<std::byte const>(&b, 1)) == 0);
    }
  }

  TEST_CASE("PcmRingBuffer - Concurrency", "[audio][unit][ring_buffer]")
  {
    auto buffer = PcmRingBuffer{};
    int const iterations = 10000;
    auto done = std::atomic<bool>{false};

    std::jthread producer(
      [&]
      {
        for (int i = 0; i < iterations; ++i)
        {
          std::byte b = static_cast<std::byte>(i % 256);

          while (buffer.write(std::span(&b, 1)) == 0)
          {
            std::this_thread::yield();
          }
        }

        done = true;
      });

    std::jthread consumer(
      [&]
      {
        int count = 0;

        while (!done || buffer.size() > 0)
        {
          std::byte b{};

          if (buffer.read(std::span(&b, 1)) == 1)
          {
            REQUIRE(b == static_cast<std::byte>(count % 256));
            count++;
          }
          else
          {
            std::this_thread::yield();
          }
        }

        REQUIRE(count == iterations);
      });

    producer.join();
    consumer.join();
  }
} // namespace ao::audio::test
