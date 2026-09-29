// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "ScriptedDecoderSession.h"
#include "lib/audio/PcmRingBuffer.h"
#include "lib/audio/PcmSource.h"
#include "lib/audio/StreamingSource.h"
#include "lib/audio/detail/RenderPath.h"
#include "lib/audio/detail/RenderTimeline.h"
#include <ao/Error.h>
#include <ao/audio/DecodedStreamInfo.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <thread>
#include <utility>
#include <vector>

namespace ao::audio::test
{
  namespace
  {
    // 24-bit stereo output: a 6-byte frame that does not divide the 2 MiB ring
    // capacity, so the growing block below must park a whole-frame remainder.
    constexpr std::size_t kFrameSize = 6;
    constexpr std::size_t kUsableRingBytes = (kRingBufferCapacity / kFrameSize) * kFrameSize;
    // Exactly the 500 ms preroll target at 288000 bytes per second.
    constexpr std::size_t kFirstBlockBytes = 144000;
    // One full ring worth of frames: larger than the space left after the
    // first block, so admission predicts a fit and the write truncates.
    constexpr std::size_t kGrowingBlockBytes = kUsableRingBytes;
    constexpr auto kPrerollDuration = std::chrono::milliseconds{500};
    constexpr auto kCompletionTimeout = std::chrono::seconds{5};

    DecodedStreamInfo alignedStreamInfo()
    {
      auto const sourceFormat = SignalFormat{.sampleRate = 48000, .channels = 2, .precisionBits = 24};
      return DecodedStreamInfo{.sourceFormat = sourceFormat,
                               .outputFormat = pcmFormat(sourceFormat, SampleEncoding::Signed24PackedLe),
                               .duration = std::chrono::seconds{1}};
    }

    std::vector<std::byte> markerBlock(std::size_t byteCount, std::byte marker)
    {
      return std::vector(byteCount, marker);
    }

    // The decoder script: the growing block exceeds the space left after the
    // first, so the background producer parks a partial write and the ring
    // holds its whole-frame capacity.
    std::vector<std::byte> expectedStream()
    {
      auto expected = markerBlock(kFirstBlockBytes, std::byte{0x10});
      auto const growing = markerBlock(kGrowingBlockBytes, std::byte{0x20});
      expected.insert(expected.end(), growing.begin(), growing.end());
      return expected;
    }

    std::unique_ptr<StreamingSource> makeParkedPartialWriteSource()
    {
      auto decoderPtr = std::make_unique<ScriptedDecoderSession>(alignedStreamInfo());
      decoderPtr->setReadScript({{markerBlock(kFirstBlockBytes, std::byte{0x10}), false},
                                 {markerBlock(kGrowingBlockBytes, std::byte{0x20}), true}});
      return std::make_unique<StreamingSource>(
        std::move(decoderPtr), alignedStreamInfo(), kPrerollDuration, std::chrono::milliseconds{10000});
    }

    // Blocks until the background producer has parked the growing block: the
    // ring content jumps from the preroll block to full capacity in one write,
    // so the full-capacity duration is observable only after the park begins.
    void waitUntilRingFull(StreamingSource& source)
    {
      auto const bytesPerSecondValue = bytesPerSecond(alignedStreamInfo().outputFormat);
      auto const ringFullDuration = std::chrono::milliseconds{(kUsableRingBytes * 1000) / bytesPerSecondValue};
      REQUIRE(ringFullDuration > kPrerollDuration);
      auto const deadline = std::chrono::steady_clock::now() + kCompletionTimeout;

      while (source.bufferedDuration() < ringFullDuration && std::chrono::steady_clock::now() < deadline)
      {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
      }

      REQUIRE(source.bufferedDuration() == ringFullDuration);
    }
  } // namespace

  TEST_CASE("StreamingSource - parked partial write keeps every read whole-frame aligned and lossless",
            "[audio][unit][streaming-source]")
  {
    REQUIRE(frameBytes(alignedStreamInfo().outputFormat) == kFrameSize);
    auto errors = std::atomic{0};
    auto sourcePtr = makeParkedPartialWriteSource();
    REQUIRE(sourcePtr->prepare());
    sourcePtr->activate([&](Error const&) { ++errors; });
    waitUntilRingFull(*sourcePtr);

    auto const expected = expectedStream();
    auto ledger = std::vector<std::byte>{};
    ledger.reserve(expected.size());

    // A starved read larger than the buffered content returns exactly the
    // whole-frame content; the parked remainder is not visible yet.
    auto output = std::vector<std::byte>(kUsableRingBytes + (kFrameSize * 2));
    auto const firstRead = sourcePtr->read(output);
    REQUIRE(firstRead == kUsableRingBytes);
    CHECK(firstRead % kFrameSize == 0);
    ledger.insert(ledger.end(), output.begin(), output.begin() + static_cast<std::ptrdiff_t>(firstRead));

    auto const deadline = std::chrono::steady_clock::now() + kCompletionTimeout;

    while (!sourcePtr->isDrained() && std::chrono::steady_clock::now() < deadline)
    {
      auto const bytesRead = sourcePtr->read(output);
      CHECK(bytesRead % kFrameSize == 0);
      ledger.insert(ledger.end(), output.begin(), output.begin() + static_cast<std::ptrdiff_t>(bytesRead));

      if (bytesRead == 0)
      {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
      }
    }

    REQUIRE(sourcePtr->isDrained());
    REQUIRE(ledger.size() == expected.size());
    CHECK(std::ranges::equal(ledger, expected));
    CHECK(errors.load() == 0);
  }

  TEST_CASE("Engine render path - starved render across a parked partial write stays frame aligned",
            "[audio][unit][engine]")
  {
    REQUIRE(frameBytes(alignedStreamInfo().outputFormat) == kFrameSize);
    auto errors = std::atomic{0};
    auto sourcePtr = makeParkedPartialWriteSource();
    REQUIRE(sourcePtr->prepare());
    sourcePtr->activate([&](Error const&) { ++errors; });
    waitUntilRingFull(*sourcePtr);

    auto timeline = detail::RenderTimeline{};
    auto nodePtr = std::make_unique<detail::RenderTimeline::Node>();
    nodePtr->sourcePtr = std::shared_ptr<PcmSource>{std::move(sourcePtr)};
    nodePtr->backendFormat = alignedStreamInfo().outputFormat;
    nodePtr->info = alignedStreamInfo();
    timeline.publishCurrent(std::move(nodePtr));

    auto engineFrameBytes = std::atomic<std::uint32_t>{kFrameSize};
    auto playbackDrainPending = std::atomic{false};
    auto const expected = expectedStream();
    auto ledger = std::vector<std::byte>{};
    ledger.reserve(expected.size());

    // A render request larger than the buffered content must yield whole
    // frames only, and the parked remainder must survive to a later render.
    auto output = std::vector<std::byte>(kUsableRingBytes + (kFrameSize * 2));
    bool drained = false;
    auto const deadline = std::chrono::steady_clock::now() + kCompletionTimeout;

    while (!drained && std::chrono::steady_clock::now() < deadline)
    {
      auto const result = detail::renderPcm(
        timeline,
        engineFrameBytes,
        playbackDrainPending,
        1,
        output,
        [](std::uint64_t) noexcept { return true; },
        [](std::uint64_t) noexcept { return false; });

      CHECK(result.bytesWritten % kFrameSize == 0);
      CHECK(result.positionFrameOffset == 0);
      CHECK(result.positionFrames == result.bytesWritten / kFrameSize);
      ledger.insert(ledger.end(), output.begin(), output.begin() + static_cast<std::ptrdiff_t>(result.bytesWritten));
      drained = result.drained;

      if (result.bytesWritten == 0 && !drained)
      {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
      }
    }

    REQUIRE(drained);
    REQUIRE(ledger.size() == expected.size());
    CHECK(std::ranges::equal(ledger, expected));
    CHECK(playbackDrainPending.load());
    CHECK(errors.load() == 0);
  }
} // namespace ao::audio::test
