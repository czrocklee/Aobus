// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "EngineTestSupport.h"
#include "FakeCapturingBackend.h"
#include "ScriptedDecoderSession.h"
#include <ao/Error.h>
#include <ao/audio/Engine.h>
#include <ao/audio/PlaybackInput.h>
#include <ao/audio/RenderTarget.h>
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
#include <filesystem>
#include <map>
#include <memory>
#include <semaphore>
#include <utility>
#include <vector>

namespace ao::audio::test
{
  TEST_CASE("Engine - Error rejects seek before and after source failure delivery",
            "[audio][regression][engine][concurrency]")
  {
    auto const guarded = GENERATE(false, true);
    auto const initiallyPaused = GENERATE(false, true);
    CAPTURE(guarded, initiallyPaused);

    auto workerEntered = std::binary_semaphore{0};
    auto workerRelease = std::binary_semaphore{0};
    auto workerFlushed = std::binary_semaphore{0};
    auto failures = std::vector<Engine::PlaybackFailure>{};
    auto endings = std::vector<Engine::TrackEnded>{};
    auto seekCount = std::atomic{std::size_t{0}};
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
} // namespace ao::audio::test
