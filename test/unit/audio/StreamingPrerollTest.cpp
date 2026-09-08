// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "ScriptedDecoderSession.h"
#include "lib/audio/PcmRingBuffer.h"
#include "lib/audio/StreamingSource.h"
#include <ao/Error.h>
#include <ao/audio/DecodedStreamInfo.h>
#include <ao/audio/DecoderSession.h>
#include <ao/audio/PcmBlock.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <semaphore>
#include <span>
#include <utility>
#include <vector>

namespace ao::audio::test
{
  namespace
  {
    constexpr std::size_t kFirstBlockByteCount = 32768;
    constexpr std::size_t kGrowingBlockByteCount = kRingBufferCapacity - 32;
    constexpr std::size_t kTailByteCount = 64;

    DecodedStreamInfo prerollStreamInfo()
    {
      auto const format = SignalFormat{.sampleRate = 192000, .channels = 8, .precisionBits = 32};
      return DecodedStreamInfo{.sourceFormat = format,
                               .outputFormat = pcmFormat(format, SampleEncoding::Signed32Le),
                               .duration = std::chrono::seconds{1}};
    }

    // Reusing one allocation makes the next read invalidate the previous PCM
    // contents, as permitted by DecoderSession's borrowed-block contract.
    class GrowingPcmDecoder final : public DecoderSession
    {
    public:
      explicit GrowingPcmDecoder(bool growingBlockEndsStream)
        : _growingBlockEndsStream{growingBlockEndsStream}, _bytes{kRingBufferCapacity}
      {
      }

      Result<> seek(std::chrono::milliseconds /*offset*/) noexcept override
      {
        _blockIndex = 0;
        ++_seekCount;
        std::ranges::fill(_bytes, std::byte{0xFF});
        return {};
      }

      void flush() noexcept override {}

      Result<PcmBlock> readNextBlock() noexcept override
      {
        auto const index = _blockIndex++;
        std::size_t byteCount = kTailByteCount;

        if (index == 0)
        {
          byteCount = kFirstBlockByteCount;
        }
        else if (index == 1)
        {
          byteCount = kGrowingBlockByteCount;
        }

        auto const value = static_cast<std::byte>((0x10U * (1U + _seekCount)) + index);
        std::ranges::fill(_bytes, value);
        return PcmBlock{.bytes = std::span<std::byte const>{_bytes}.first(byteCount),
                        .frames = static_cast<std::uint32_t>(byteCount / 32),
                        .endOfStream = index >= (_growingBlockEndsStream ? 1U : 2U)};
      }

      DecodedStreamInfo streamInfo() const noexcept override { return prerollStreamInfo(); }

    private:
      bool _growingBlockEndsStream;
      std::vector<std::byte> _bytes;
      std::size_t _blockIndex = 0;
      std::size_t _seekCount = 0;
    };

    std::vector<std::byte> expectedPcm(bool growingBlockEndsStream, std::uint8_t firstBlockValue)
    {
      auto bytes = std::vector(kFirstBlockByteCount, static_cast<std::byte>(firstBlockValue));
      bytes.insert(bytes.end(), kGrowingBlockByteCount, static_cast<std::byte>(firstBlockValue + 1U));

      if (!growingBlockEndsStream)
      {
        bytes.insert(bytes.end(), kTailByteCount, static_cast<std::byte>(firstBlockValue + 2U));
      }

      return bytes;
    }

    void checkPcmUntilDrained(StreamingSource& source, std::span<std::byte const> expected)
    {
      auto output = std::vector<std::byte>(expected.size() + 32);
      std::size_t byteCount = 0;
      auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};

      // Drive the real consumer; the deadline only bounds a broken producer.
      // Success depends on exact PCM and terminal state, never elapsed time.
      while (!source.isDrained() && byteCount < output.size() && std::chrono::steady_clock::now() < deadline)
      {
        byteCount += source.read(std::span{output}.subspan(byteCount));
      }

      REQUIRE(source.isDrained());
      REQUIRE(byteCount == expected.size());
      CHECK(std::ranges::equal(std::span{output}.first(byteCount), expected));
      CHECK(source.bufferedDuration() == std::chrono::milliseconds{0});
    }
  } // namespace

  TEST_CASE("StreamingSource - growing preroll returns without a consumer and preserves all PCM",
            "[audio][regression][streaming-preroll][concurrency]")
  {
    bool const growingBlockEndsStream = GENERATE(false, true);
    auto errors = std::atomic{0};
    auto source = StreamingSource{std::make_unique<GrowingPcmDecoder>(growingBlockEndsStream),
                                  prerollStreamInfo(),
                                  std::chrono::milliseconds{500},
                                  std::chrono::milliseconds{500}};

    REQUIRE(source.prepare());
    CHECK_FALSE(source.isDrained());

    // Empty the ring before activation: an EOF on the retained block must not
    // report drainage or prevent the worker from delivering that block's tail.
    auto prefix = std::vector<std::byte>(kRingBufferCapacity);
    REQUIRE(source.read(prefix) == prefix.size());
    CHECK_FALSE(source.isDrained());
    auto const expected = expectedPcm(growingBlockEndsStream, 0x10);
    CHECK(std::ranges::equal(prefix, std::span{expected}.first(prefix.size())));

    source.activate([&](Error const&) { ++errors; });
    checkPcmUntilDrained(source, std::span{expected}.subspan(prefix.size()));
    CHECK(errors.load() == 0);
  }

  TEST_CASE("StreamingSource - seek retires pending PCM and completes growing preroll without a consumer",
            "[audio][regression][streaming-preroll][concurrency]")
  {
    bool const growingBlockEndsStream = GENERATE(false, true);
    auto errors = std::atomic{0};
    auto source = StreamingSource{std::make_unique<GrowingPcmDecoder>(growingBlockEndsStream),
                                  prerollStreamInfo(),
                                  std::chrono::milliseconds{500},
                                  std::chrono::milliseconds{500}};
    REQUIRE(source.prepare());
    source.activate([&](Error const&) { ++errors; });

    // The old ring remains full, so its producer cannot finish the retained
    // block. seek must join it and discard its borrow before decoder mutation.
    REQUIRE(source.seek(std::chrono::milliseconds{50}));
    checkPcmUntilDrained(source, expectedPcm(growingBlockEndsStream, 0x20));
    CHECK(errors.load() == 0);
  }

  TEST_CASE("StreamingSource - consecutive seeks retire every pending block before the final PCM is consumed",
            "[audio][regression][streaming-preroll][concurrency]")
  {
    bool const growingBlockEndsStream = GENERATE(false, true);
    auto errors = std::atomic{0};
    auto source = StreamingSource{std::make_unique<GrowingPcmDecoder>(growingBlockEndsStream),
                                  prerollStreamInfo(),
                                  std::chrono::milliseconds{500},
                                  std::chrono::milliseconds{500}};
    REQUIRE(source.prepare());
    auto const fullBufferDuration = source.bufferedDuration();
    source.activate([&](Error const&) { ++errors; });

    // Withhold consumption throughout: every seek fills the ring and retains
    // an unwritten tail that the following seek must retire before decoder reuse.
    for (std::int32_t seekIndex = 1; seekIndex <= 8; ++seekIndex)
    {
      CAPTURE(seekIndex);
      REQUIRE(source.seek(std::chrono::milliseconds{50 * seekIndex}));
      CHECK(source.bufferedDuration() == fullBufferDuration);
      CHECK_FALSE(source.isDrained());
    }

    // Each seek changes all PCM markers. Only the eighth seek's 0x90/0x91
    // blocks (and optional 0x92 tail) may survive to consumption.
    checkPcmUntilDrained(source, expectedPcm(growingBlockEndsStream, 0x90));
    CHECK(errors.load() == 0);
  }

  TEST_CASE("StreamingSource - destruction stops a pending preroll write without a consumer",
            "[audio][regression][streaming-preroll][concurrency]")
  {
    bool const activate = GENERATE(false, true);
    auto destroyedPtr = std::make_shared<std::atomic<std::size_t>>(0);
    auto errors = std::atomic{0};
    auto decoderPtr = std::make_unique<ScriptedDecoderSession>(prerollStreamInfo());
    decoderPtr->setDestroyCounter(destroyedPtr);
    decoderPtr->setReadScript({{std::vector(kFirstBlockByteCount, std::byte{0x10}), false},
                               {std::vector(kGrowingBlockByteCount, std::byte{0x11}), true}});
    auto sourcePtr = std::make_unique<StreamingSource>(
      std::move(decoderPtr), prerollStreamInfo(), std::chrono::milliseconds{500}, std::chrono::milliseconds{500});
    REQUIRE(sourcePtr->prepare());

    if (activate)
    {
      sourcePtr->activate([&](Error const&) { ++errors; });
    }

    sourcePtr.reset();
    CHECK(destroyedPtr->load() == 1);
    CHECK(errors.load() == 0);
  }

  TEST_CASE("StreamingSource - active growing block write remains stoppable without a consumer",
            "[audio][regression][streaming-preroll][concurrency]")
  {
    auto growingRead = std::binary_semaphore{0};
    auto destroyedPtr = std::make_shared<std::atomic<std::size_t>>(0);
    auto errors = std::atomic{0};
    auto decoderPtr = std::make_unique<ScriptedDecoderSession>(prerollStreamInfo());
    decoderPtr->setDestroyCounter(destroyedPtr);
    decoderPtr->setReadScript({{std::vector(kFirstBlockByteCount, std::byte{0x10}), false},
                               {std::vector(kGrowingBlockByteCount, std::byte{0x11}), true}});
    decoderPtr->setReadObserver(
      [&](std::size_t count)
      {
        if (count == 2)
        {
          growingRead.release();
        }
      });
    auto sourcePtr = std::make_unique<StreamingSource>(
      std::move(decoderPtr), prerollStreamInfo(), std::chrono::milliseconds{1}, std::chrono::milliseconds{500});
    REQUIRE(sourcePtr->prepare());
    sourcePtr->activate([&](Error const&) { ++errors; });
    REQUIRE(growingRead.try_acquire_for(std::chrono::seconds{5}));

    sourcePtr.reset();
    CHECK(destroyedPtr->load() == 1);
    CHECK(errors.load() == 0);
  }
} // namespace ao::audio::test
