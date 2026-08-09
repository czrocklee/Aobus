// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/detail/EngineRtSignalRing.h"

#include "lib/audio/detail/EngineEventQueueInvariants.h"
#include "lib/audio/detail/RenderPath.h"
#include "lib/audio/detail/RenderTimeline.h"
#include "test/unit/audio/EngineTestSupport.h"
#include "test/unit/audio/FakeCapturingBackend.h"
#include <ao/Error.h>
#include <ao/audio/Engine.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/PcmSource.h>
#include <ao/audio/PlaybackInput.h>
#include <ao/audio/RenderTarget.h>
#include <ao/audio/SampleEncoding.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <semaphore>
#include <span>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace ao::audio::test
{
  namespace
  {
    enum class ProbeSignalKind : std::uint8_t
    {
      Spliced,
      Drained,
    };

    struct ProbeSignal final
    {
      ProbeSignalKind kind = ProbeSignalKind::Drained;
      std::uint32_t sequence = 0;
    };

    class FinitePcmSource final : public PcmSource
    {
    public:
      explicit FinitePcmSource(std::array<std::byte, 2> data)
        : _data{data}
      {
      }

      Result<> seek(std::chrono::milliseconds /*offset*/) noexcept override
      {
        _offset = 0;
        return {};
      }

      std::size_t read(std::span<std::byte> output) noexcept override
      {
        auto const readSize = std::min(output.size(), _data.size() - _offset);
        std::copy_n(_data.data() + _offset, readSize, output.data());
        _offset += readSize;
        return readSize;
      }

      bool isDrained() const noexcept override { return _offset == _data.size(); }

      std::chrono::milliseconds bufferedDuration() const noexcept override { return {}; }

    private:
      std::array<std::byte, 2> _data;
      std::size_t _offset = 0;
    };

    std::unique_ptr<detail::RenderTimeline::Node> makeTimelineNode(std::byte first, std::byte second)
    {
      auto const format = PcmFormat{.sampleRate = 1000, .channels = 1, .encoding = SampleEncoding::Signed16Le};
      auto nodePtr = std::make_unique<detail::RenderTimeline::Node>();
      nodePtr->sourcePtr = std::make_shared<FinitePcmSource>(std::array{first, second});
      nodePtr->backendFormat = format;
      nodePtr->info.outputFormat = format;
      return nodePtr;
    }
  } // namespace

  TEST_CASE("Engine RT signal ring - legal splice and drain fill the exact capacity",
            "[audio][unit][engine][concurrency]")
  {
    auto ring = detail::EngineRtSignalRing<ProbeSignal>{};
    static_assert(decltype(ring)::kCapacity == 2);

    ring.push({.kind = ProbeSignalKind::Spliced, .sequence = 1});
    ring.push({.kind = ProbeSignalKind::Drained, .sequence = 2});
    CHECK(ring.readAvailable() == 2);

    auto signal = ProbeSignal{};
    REQUIRE(ring.pop(signal));
    CHECK(signal.kind == ProbeSignalKind::Spliced);
    CHECK(signal.sequence == 1);
    REQUIRE(ring.pop(signal));
    CHECK(signal.kind == ProbeSignalKind::Drained);
    CHECK(signal.sequence == 2);
    CHECK_FALSE(ring.pop(signal));
  }

  TEST_CASE("Engine RT signal ring - serialized producer ownership may hand off between threads",
            "[audio][unit][engine][concurrency]")
  {
    auto ring = detail::EngineRtSignalRing<ProbeSignal>{};

    auto firstProducer = std::jthread{[&] { ring.push({.kind = ProbeSignalKind::Spliced, .sequence = 1}); }};
    firstProducer.join();
    auto secondProducer = std::jthread{[&] { ring.push({.kind = ProbeSignalKind::Drained, .sequence = 2}); }};
    secondProducer.join();

    auto signal = ProbeSignal{};
    REQUIRE(ring.pop(signal));
    CHECK(signal.sequence == 1);
    REQUIRE(ring.pop(signal));
    CHECK(signal.sequence == 2);
  }

  TEST_CASE("Engine render path - sequential splices never accumulate two splice signals",
            "[audio][unit][engine][concurrency]")
  {
    auto timeline = detail::RenderTimeline{};
    timeline.publishCurrent(makeTimelineNode(std::byte{0x11}, std::byte{0x12}));
    timeline.armLookahead(makeTimelineNode(std::byte{0x21}, std::byte{0x22}));
    auto thirdNodePtr = makeTimelineNode(std::byte{0x31}, std::byte{0x32});

    auto spliceHandoffInProgress = std::atomic<bool>{false};
    auto accumulatedFrames = std::atomic<std::uint64_t>{0};
    auto engineSampleRate = std::atomic<std::uint32_t>{1000};
    auto engineFrameBytes = std::atomic<std::uint32_t>{2};
    auto underrunCount = std::atomic<std::uint32_t>{0};
    auto playbackDrainPending = std::atomic<bool>{false};
    std::size_t pendingSignals = 0;
    std::size_t maximumPendingSignals = 0;
    std::size_t spliceCount = 0;

    auto output = std::array<std::byte, 6>{};
    auto const result = detail::renderPcm(
      timeline,
      engineFrameBytes,
      playbackDrainPending,
      1,
      output,
      [](std::uint64_t) noexcept { return true; },
      [&](std::uint64_t const generation) noexcept
      {
        auto* signaledNode = static_cast<detail::RenderTimeline::Node*>(nullptr);
        auto const spliced = detail::trySplicePreparedNext(
          timeline,
          spliceHandoffInProgress,
          generation,
          [](std::uint64_t) noexcept { return true; },
          accumulatedFrames,
          engineSampleRate,
          engineFrameBytes,
          underrunCount,
          playbackDrainPending,
          [&](std::uint64_t, detail::RenderTimeline::Node* node) noexcept
          {
            signaledNode = node;
            ++pendingSignals;
            maximumPendingSignals = std::max(maximumPendingSignals, pendingSignals);
          });

        if (signaledNode != nullptr)
        {
          [[maybe_unused]] auto retiredNodePtr = timeline.promoteSplicedLookahead(signaledNode);
          --pendingSignals;
          ++spliceCount;

          if (thirdNodePtr)
          {
            timeline.armLookahead(std::move(thirdNodePtr));
          }
        }

        return spliced;
      });

    CHECK(result.bytesWritten == output.size());
    CHECK(
      output ==
      std::array{std::byte{0x11}, std::byte{0x12}, std::byte{0x21}, std::byte{0x22}, std::byte{0x31}, std::byte{0x32}});
    CHECK(spliceCount == 2);
    CHECK(maximumPendingSignals == 1);
    CHECK(pendingSignals == 0);
  }

  TEST_CASE("Engine lookahead - a pending drain signal does not prohibit an empty-owner arm",
            "[audio][unit][engine][concurrency]")
  {
    auto ring = detail::EngineRtSignalRing<ProbeSignal>{};
    ring.push({.kind = ProbeSignalKind::Drained, .sequence = 1});

    auto timeline = detail::RenderTimeline{};
    auto nodePtr = std::make_unique<detail::RenderTimeline::Node>();
    auto* const node = nodePtr.get();
    timeline.armLookahead(std::move(nodePtr));
    CHECK(timeline.lookaheadNode() == node);

    auto signal = ProbeSignal{};
    REQUIRE(ring.pop(signal));
    CHECK(signal.kind == ProbeSignalKind::Drained);

    auto* const disarmed = timeline.disarmLookahead();
    REQUIRE(disarmed == node);
    timeline.dropDisarmedLookahead(disarmed);
  }

  TEST_CASE("Engine event queue - settled destruction state satisfies every invariant", "[audio][unit][engine]")
  {
    detail::verifyEngineEventQueueDestruction({});
  }

  TEST_CASE("Engine RT signal ring - blocked consumer accepts the legal splice-drain full sequence",
            "[audio][integration][engine][concurrency]")
  {
    auto backendPtr = std::make_unique<FakeCapturingBackend>();
    auto* const backend = backendPtr.get();
    auto const format = PcmFormat{.sampleRate = 1000, .channels = 1, .encoding = SampleEncoding::Signed16Le};
    auto engine = Engine{
      std::move(backendPtr),
      makeEngineTestDevice(),
      makePathScriptedDecoderFactory({
        {.path = "first.flac", .info = makeScriptedStreamInfo(format), .data = {std::byte{0x11}, std::byte{0x12}}},
        {.path = "second.flac", .info = makeScriptedStreamInfo(format), .data = {std::byte{0x21}, std::byte{0x22}}},
      })};

    auto callbacks = std::vector<std::string_view>{};
    callbacks.reserve(2);
    auto ended = CallbackLatch{};
    engine.setOnTrackAdvanced([&](Engine::TrackAdvanced const&) { callbacks.emplace_back("advanced"); });
    engine.setOnTrackEnded(
      [&](Engine::TrackEnded const&)
      {
        callbacks.emplace_back("ended");
        ended.notify();
      });

    engine.play(makePlaybackItem(PlaybackInput{.filePath = "first.flac"}));
    REQUIRE(engine.setNext(makePlaybackItem(PlaybackInput{.filePath = "second.flac"})));
    auto* const target = backend->target();
    REQUIRE(target != nullptr);

    auto eventWorkerEntered = std::binary_semaphore{0};
    auto holdEventWorker = std::binary_semaphore{0};
    engine.defer(
      [&]
      {
        eventWorkerEntered.release();
        holdEventWorker.acquire();
      });
    eventWorkerEntered.acquire();

    auto combinedOutput = std::array<std::byte, 4>{};
    auto const combinedResult = target->renderPcm(combinedOutput);
    auto drainedOutput = std::array<std::byte, 2>{};
    auto const drainedResult = target->renderPcm(drainedOutput);
    backend->emitDrainComplete();
    holdEventWorker.release();

    CHECK(combinedResult.bytesWritten == combinedOutput.size());
    CHECK_FALSE(combinedResult.drained);
    CHECK(drainedResult.bytesWritten == 0);
    CHECK(drainedResult.drained);
    REQUIRE(ended.waitForCount(1));
    REQUIRE(callbacks.size() == 2);
    CHECK(callbacks[0] == "advanced");
    CHECK(callbacks[1] == "ended");
  }
} // namespace ao::audio::test
