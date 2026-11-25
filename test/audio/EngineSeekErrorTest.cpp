// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "EngineTestSupport.h"
#include "FakeCapturingBackend.h"
#include "ScriptedDecoderSession.h"
#include <ao/Error.h>
#include <ao/audio/Engine.h>
#include <ao/audio/PlaybackInput.h>
#include <ao/audio/RenderTarget.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/Transport.h>
#include <ao/utility/ScopedRegistration.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <semaphore>
#include <stop_token>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace ao::audio::test
{
  TEST_CASE("Engine - Error rejects seek before and after source failure delivery", "[concurrency]")
  {
    auto const guarded = GENERATE(false, true);
    auto const initiallyPaused = GENERATE(false, true);
    CAPTURE(guarded, initiallyPaused);

    auto workerEntered = std::binary_semaphore{0};
    auto workerRelease = std::binary_semaphore{0};
    auto workerFlushed = std::binary_semaphore{0};
    auto failures = std::vector<Engine::PlaybackFailure>{};
    auto endings = std::vector<Engine::TrackEnded>{};
    auto seekCount = std::atomic<std::size_t>{0};
    auto registryPtr = std::make_shared<std::map<std::filesystem::path, ScriptedDecoderSession*>>();
    auto const format = makeEngineTestFormat();
    auto const failedData = std::vector<std::byte>(4096, std::byte{0x11});
    auto const replacementData = std::vector<std::byte>(4096, std::byte{0x22});
    auto const item = makePlaybackItem(PlaybackInput{.filePath = "seek-error.flac"});
    auto const replacement = makePlaybackItem(PlaybackInput{.filePath = "replacement.flac"});
    auto backendPtr = std::make_unique<FakeCapturingBackend>();
    auto* const backend = backendPtr.get();
    auto engine =
      Engine{std::move(backendPtr),
             makeEngineTestDevice(),
             makeRegisteringDecoderFactory(
               {{.track = {.path = item.input.filePath, .info = makeScriptedStreamInfo(format), .data = failedData},
                 .optSeekScript = {}},
                {.track = {.path = replacement.input.filePath,
                           .info = makeScriptedStreamInfo(format),
                           .data = replacementData},
                 .optSeekScript = {}}},
               registryPtr)};
    engine.setOnPlaybackFailure([&](Engine::PlaybackFailure const& failure) { failures.push_back(failure); });
    engine.setOnTrackEnded([&](Engine::TrackEnded const& event) { endings.push_back(event); });
    engine.play(item);
    REQUIRE(engine.transport() == Transport::Playing);
    auto const generation = engine.playbackGeneration();

    if (initiallyPaused)
    {
      engine.pause();
      REQUIRE(engine.transport() == Transport::Paused);
    }

    // Keep the source alive after the synchronous seek failure while its error
    // event waits behind this notification. Release even if an assertion fails.
    auto releaseWorker = utility::ScopedRegistration{[&] { workerRelease.release(); }};
    engine.defer(
      [&]
      {
        workerEntered.release();
        workerRelease.acquire();
      });
    REQUIRE(workerEntered.try_acquire_for(std::chrono::seconds{5}));

    auto* const decoder = registryPtr->at(item.input.filePath);
    decoder->setSeekObserver([&](std::chrono::milliseconds) { seekCount.fetch_add(1, std::memory_order_relaxed); });
    decoder->setSeekResult(makeError(Error::Code::SeekFailed, "seek failed"));

    if (guarded)
    {
      // True means issuance, even though the source reports an audio failure.
      CHECK(engine.trySeek(item.id, std::chrono::milliseconds{50}));
    }
    else
    {
      engine.seek(std::chrono::milliseconds{50});
    }

    auto const failedStatus = engine.status();
    REQUIRE(failedStatus.transport == Transport::Error);
    CHECK(failedStatus.statusText == "seek failed");
    CHECK(failedStatus.elapsed == std::chrono::milliseconds{50});
    REQUIRE(engine.isCurrentPlaybackItem(item.id));
    REQUIRE(backend->target() != nullptr);
    REQUIRE(seekCount.load(std::memory_order_relaxed) == 1);

    // A retry would succeed at the decoder. Error must reject at admission,
    // before Buffering erases it or stop/flush/seek can restart the backend.
    decoder->setSeekResult({});
    backend->clearEvents();

    if (guarded)
    {
      CHECK_FALSE(engine.trySeek(item.id, std::chrono::milliseconds{100}));
    }
    else
    {
      engine.seek(std::chrono::milliseconds{100});
    }

    auto const rejectedStatus = engine.status();
    CHECK(rejectedStatus.transport == Transport::Error);
    CHECK(rejectedStatus.statusText == failedStatus.statusText);
    CHECK(rejectedStatus.elapsed == failedStatus.elapsed);
    CHECK(rejectedStatus.duration == failedStatus.duration);
    CHECK(rejectedStatus.bufferedDuration == failedStatus.bufferedDuration);
    CHECK(engine.playbackGeneration() == generation);
    CHECK(engine.isCurrentPlaybackItem(item.id));
    CHECK(seekCount.load(std::memory_order_relaxed) == 1);
    CHECK(backend->events().empty());

    releaseWorker.reset();
    engine.defer([&] { workerFlushed.release(); });
    REQUIRE(workerFlushed.try_acquire_for(std::chrono::seconds{5}));
    REQUIRE(failures.size() == 1);
    CHECK(failures.front().kind == Engine::PlaybackFailureKind::Decode);
    CHECK(failures.front().itemId == item.id);
    CHECK(failures.front().generation == generation);
    CHECK(failures.front().error.code == Error::Code::SeekFailed);
    CHECK(failures.front().error.message == "seek failed");
    REQUIRE(endings.size() == 1);
    CHECK(endings.front().generation == generation);
    CHECK(engine.transport() == Transport::Error);
    CHECK_FALSE(engine.isCurrentPlaybackItem(item.id));
    CHECK(backend->target() == nullptr);
    backend->clearEvents();

    if (guarded)
    {
      CHECK_FALSE(engine.trySeek(item.id, std::chrono::milliseconds{200}));
    }
    else
    {
      engine.seek(std::chrono::milliseconds{200});
    }

    auto const settledStatus = engine.status();
    CHECK(settledStatus.transport == Transport::Error);
    CHECK(settledStatus.statusText == "seek failed");
    CHECK(settledStatus.elapsed == std::chrono::milliseconds{0});
    CHECK(seekCount.load(std::memory_order_relaxed) == 1);
    CHECK(backend->events().empty());

    // Recovery creates a new playback, not a seek on the failed source.
    engine.play(replacement);
    REQUIRE(engine.transport() == Transport::Playing);
    CHECK(engine.playbackGeneration() > generation);
    CHECK(engine.isCurrentPlaybackItem(replacement.id));
    CHECK(engine.status().statusText.empty());
    auto* const target = backend->target();
    REQUIRE(target != nullptr);
    auto output = std::array<std::byte, 16>{};
    REQUIRE(target->renderPcm(output).bytesWritten == output.size());
    CHECK(std::ranges::all_of(output, [](std::byte const value) { return value == std::byte{0x22}; }));
  }

  TEST_CASE("Engine - deferred positioning waits for preroll and honors the latest transport command", "[concurrency]")
  {
    auto const initial = GENERATE(false, true);
    CAPTURE(initial);
    constexpr auto kOffset = std::chrono::milliseconds{10};
    auto preroll = SeekTestGate{};
    auto workerToken = std::stop_token{};
    auto workerExit = std::promise<void>{};
    auto const workerExited = workerExit.get_future();
    auto settled = CallbackLatch{};
    auto eventGate = SeekTestGate{};
    auto const item = makePlaybackItem("position.flac");
    auto backendPtr = std::make_unique<FakeCapturingBackend>();
    auto* const backend = backendPtr.get();
    backend->setPrewarmEncoding(SampleEncoding::Signed16Le);
    backend->setSelectedEncoding(SampleEncoding::Signed16Le);
    auto const starts = [&]
    { return std::ranges::count(backend->events(), std::string{"start"}, &FakeCapturingBackend::Event::name); };
    auto engine = Engine{
      std::move(backendPtr),
      makeEngineTestDevice(),
      [&](std::filesystem::path const&, std::optional<SampleEncoding>)
      {
        auto decoderPtr = std::make_unique<ScriptedDecoderSession>(makeScriptedStreamInfo(makeEngineTestFormat()));
        decoderPtr->setReadScript({{.data = std::vector(16, std::byte{0x11}), .endOfStream = true}});
        decoderPtr->setWorkerSeek(kOffset,
                                  [&](std::stop_token token) -> Result<>
                                  {
                                    workerToken = token;
                                    return {};
                                  });
        // 500 ms in the fixture format. The worker publishes ready before the
        // following steady-state EOF read; thread exit fences both publications.
        decoderPtr->setSeekReadScript({{.data = std::vector(88200, std::byte{0x22})}, {.endOfStream = true}});
        decoderPtr->setReadObserver(
          [&, prerollRead = initial ? 1U : 2U](std::size_t read)
          {
            if (read == prerollRead)
            {
              std::ignore = preroll.enterAndWait(workerToken);
            }
            else if (read == prerollRead + 1)
            {
              workerExit.set_value_at_thread_exit();
            }
          });
        return decoderPtr;
      }};

    if (initial)
    {
      auto stagedRes = engine.stagePlayback(item, kOffset);
      REQUIRE(stagedRes);
      auto const receiptRes = engine.commitPlayback(std::move(*stagedRes));
      REQUIRE(receiptRes);
      CHECK(receiptRes->playbackStarted);
    }
    else
    {
      engine.play(item);
      REQUIRE(engine.transport() == Transport::Playing);
      backend->clearEvents();
      REQUIRE(engine.trySeek(item.id, kOffset));
    }

    REQUIRE(preroll.tryWaitForEntry());
    CHECK(engine.transport() == Transport::Buffering);
    CHECK(engine.status().elapsed == kOffset);
    CHECK(starts() == 0);
    engine.pause();
    CHECK(engine.transport() == Transport::Paused);
    engine.resume();
    CHECK(engine.transport() == Transport::Buffering);
    CHECK(starts() == 0);

    auto releaseEvents = utility::ScopedRegistration{[&] { eventGate.release(); }};
    engine.defer([&] { std::ignore = eventGate.enterAndWait(); });
    REQUIRE(eventGate.tryWaitForEntry());
    auto const finishPreroll = [&]
    {
      preroll.release();
      REQUIRE(workerExited.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
    };
    auto expected = Transport::Playing;
    std::int32_t expectedStarts = 1;

    SECTION("Resume while pending starts once when ready")
    {
      finishPreroll();
      CHECK(engine.transport() == Transport::Buffering);
    }

    SECTION("A later pause leaves the ready source stopped")
    {
      engine.pause();
      expected = Transport::Paused;
      expectedStarts = 0;
      finishPreroll();
    }

    SECTION("A queued ready event cannot restart or finish the run after a replacement seek")
    {
      finishPreroll();
      REQUIRE(engine.trySeek(item.id, std::chrono::milliseconds{1}));
      REQUIRE(starts() == 1);
      // Model a device that accepted the entire replacement buffer but has
      // not reported hardware drain. The obsolete ready must not end it.
      auto data = std::vector<std::byte>(88200);
      REQUIRE(backend->target()->renderPcm(data).bytesWritten == data.size());
    }

    SECTION("A late ready from the same attempt cannot finish a paused device with an outstanding drain")
    {
      finishPreroll();
      engine.pause();
      engine.resume(); // Settle this ready attempt from the command before its event is delivered.
      REQUIRE(starts() == 1);
      auto* const target = backend->target();
      REQUIRE(target != nullptr);
      auto data = std::vector<std::byte>(88200);
      REQUIRE(target->renderPcm(data).bytesWritten == data.size());
      REQUIRE(target->renderPcm(data).drained);
      engine.pause(); // The device has not acknowledged drain.
      expected = Transport::Paused;
    }

    releaseEvents.reset();
    engine.defer([&] { settled.notify(); });
    REQUIRE(settled.tryWaitForCount(1));
    CHECK(preroll.outcome() == SeekTestGate::Outcome::Released);
    CHECK(eventGate.outcome() == SeekTestGate::Outcome::Released);
    CHECK(engine.transport() == expected);
    CHECK(starts() == expectedStarts);
    CHECK(backend->target() != nullptr);

    if (expected == Transport::Paused)
    {
      engine.resume();
      CHECK(engine.transport() == Transport::Playing);
      CHECK(starts() == 1);
    }
  }

  TEST_CASE("Engine - deferred seek reaches EOF or fails without starting the backend", "[concurrency]")
  {
    constexpr auto kOffset = std::chrono::milliseconds{5};
    auto gate = SeekTestGate{};
    auto settled = CallbackLatch{};
    auto ended = CallbackLatch{};
    auto workerToken = std::stop_token{};
    auto const fail = GENERATE(false, true);
    auto failures = std::vector<Engine::PlaybackFailure>{};
    auto const item = makePlaybackItem("position-outcome.flac");
    auto registryPtr = std::make_shared<std::map<std::filesystem::path, ScriptedDecoderSession*>>();
    auto backendPtr = std::make_unique<FakeCapturingBackend>();
    auto* const backend = backendPtr.get();
    auto engine =
      Engine{std::move(backendPtr),
             makeEngineTestDevice(),
             makeRegisteringDecoderFactory({{.track = {.path = item.input.filePath,
                                                       .info = makeScriptedStreamInfo(makeEngineTestFormat()),
                                                       .data = std::vector(16, std::byte{0x11})},
                                             .optSeekScript = {}}},
                                           registryPtr)};
    engine.setOnTrackEnded([&](Engine::TrackEnded const&) { ended.notify(); });
    engine.setOnPlaybackFailure([&](Engine::PlaybackFailure const& failure) { failures.push_back(failure); });
    engine.play(item);
    engine.pause();
    auto* const decoder = registryPtr->at(item.input.filePath);
    decoder->setSeekReadScript({});
    decoder->setWorkerSeek(kOffset,
                           [&](std::stop_token token) -> Result<>
                           {
                             workerToken = token;
                             return {};
                           });
    // Park the final seek after the scripted decoder's last cancellation check.
    decoder->setSeekObserver([&](std::chrono::milliseconds) { std::ignore = gate.enterAndWait(workerToken); });

    if (fail)
    {
      decoder->setSeekResult(makeError(Error::Code::SeekFailed, "position failed"));
    }

    backend->clearEvents();
    REQUIRE(engine.trySeek(item.id, kOffset));
    REQUIRE(gate.tryWaitForEntry());
    REQUIRE(engine.transport() == Transport::Paused);

    SECTION("Settlement while paused")
    {
      gate.release();
      REQUIRE(ended.tryWaitForCount(1));
      CHECK(gate.outcome() == SeekTestGate::Outcome::Released);
      CHECK(engine.transport() == (fail ? Transport::Error : Transport::Idle));

      if (fail)
      {
        REQUIRE(failures.size() == 1);
        CHECK(failures.front().itemId == item.id);
        CHECK(failures.front().error.code == Error::Code::SeekFailed);
      }
      else
      {
        CHECK(failures.empty());
      }
    }

    SECTION("Stop cancels the old attempt without a failure or natural end")
    {
      engine.stop();
      CHECK(gate.outcome() == SeekTestGate::Outcome::Cancelled);
      engine.defer([&] { settled.notify(); });
      REQUIRE(settled.tryWaitForCount(1));
      CHECK(engine.transport() == Transport::Idle);
      CHECK(failures.empty());
      CHECK(ended.count() == 0);
    }

    CHECK(std::ranges::count(backend->events(), std::string{"start"}, &FakeCapturingBackend::Event::name) == 0);
  }
} // namespace ao::audio::test
