// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "EngineTestSupport.h"
#include "FakeCapturingBackend.h"
#include <ao/Error.h>
#include <ao/audio/Engine.h>
#include <ao/audio/PlaybackInput.h>
#include <ao/audio/RenderTarget.h>
#include <ao/audio/Transport.h>
#include <ao/utility/ScopedRegistration.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <semaphore>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::audio::test
{
  namespace
  {
    constexpr auto kControlledStopError = std::string_view{"controlled stop error"};
  } // namespace

  // Not native SDK failure tests: FakeCapturingBackend::stop() always succeeds.
  // The event observer injects the terminal error callback a native backend may
  // report from inside the stop a control command issues (for example a failed
  // native stop under Core Audio). The assertions prove how Engine treats such
  // a delivered error around its own stop/flush/start sequencing; they do not
  // demonstrate an actual native SDK stop failure or its recoverability.
  TEST_CASE("Engine - terminal backend error reported during seek stop persists after successful flush and start",
            "[concurrency]")
  {
    auto workerEntered = std::binary_semaphore{0};
    auto workerRelease = std::binary_semaphore{0};
    auto workerFlushed = std::binary_semaphore{0};
    auto failures = std::vector<Engine::PlaybackFailure>{};
    auto injectedErrorCount = std::atomic<std::size_t>{0};
    auto armed = std::atomic<bool>{true};
    auto const format = makeEngineTestFormat();
    auto const data = std::vector<std::byte>(4096, std::byte{0x11});
    auto const item = makePlaybackItem(PlaybackInput{.filePath = "controlled-stop.flac"});
    auto backendPtr = std::make_unique<FakeCapturingBackend>();
    auto* const backend = backendPtr.get();
    auto engine = Engine{std::move(backendPtr),
                         makeEngineTestDevice(),
                         makePathScriptedDecoderFactory(
                           {{.path = item.input.filePath, .info = makeScriptedStreamInfo(format), .data = data}})};

    engine.setOnPlaybackFailure([&](Engine::PlaybackFailure const& failure) { failures.push_back(failure); });
    engine.play(item);
    REQUIRE(engine.transport() == Transport::Playing);
    auto const generation = engine.playbackGeneration();

    // FakeCapturingBackend::recordEvent invokes the observer while the fake
    // mutex is held, so the observer must not call backend->target(),
    // emitBackendError(), or any other fake query that locks. The live target
    // is therefore captured before the observer is installed, and the
    // callback-origin external target call is made directly on it — the same
    // seam emitBackendError uses, minus the lock it would deadlock on. The
    // once flag disarms the injection so the raw target pointer is never
    // touched again after close() or during later engine cleanup.
    auto* const target = backend->target();
    REQUIRE(target != nullptr);
    backend->setEventObserver(
      [target, &armed, &injectedErrorCount](std::string_view name)
      {
        if (name == "stop" && armed.exchange(false, std::memory_order_acq_rel))
        {
          injectedErrorCount.fetch_add(1, std::memory_order_relaxed);
          target->handleBackendError(kControlledStopError);
        }
      });
    backend->clearEvents();

    // Hold the event worker so the error queued during the seek cannot be
    // reduced until after the seek command returns. Release even if an
    // assertion fails.
    auto releaseWorker = utility::ScopedRegistration{[&] { workerRelease.release(); }};
    engine.defer(
      [&]
      {
        workerEntered.release();
        workerRelease.acquire();
      });
    REQUIRE(workerEntered.try_acquire_for(std::chrono::seconds{5}));

    engine.seek(std::chrono::milliseconds{50});

    // The seek itself completed: its stop queued the injected error, and the
    // later flush and start both succeeded, leaving playback running.
    REQUIRE(injectedErrorCount.load(std::memory_order_relaxed) == 1);
    REQUIRE(backend->events().size() == 3);
    CHECK(backend->events().at(0).name == "stop");
    CHECK(backend->events().at(1).name == "flush");
    CHECK(backend->events().at(2).name == "start");
    auto const seekStatus = engine.status();
    CHECK(seekStatus.transport == Transport::Playing);
    CHECK(seekStatus.elapsed == std::chrono::milliseconds{50});

    // Release the worker and flush the event queue with one more public defer:
    // once that marker runs, the queued error has settled.
    releaseWorker.reset();
    engine.defer([&] { workerFlushed.release(); });
    REQUIRE(workerFlushed.try_acquire_for(std::chrono::seconds{5}));

    REQUIRE(failures.size() == 1);
    CHECK(failures.front().kind == Engine::PlaybackFailureKind::DeviceLost);
    CHECK(failures.front().itemId == item.id);
    CHECK(failures.front().generation == generation);
    CHECK(failures.front().error.code == Error::Code::IoError);
    CHECK(failures.front().error.message == kControlledStopError);
    CHECK_FALSE(failures.front().recoverable);
    auto const errorStatus = engine.status();
    CHECK(errorStatus.transport == Transport::Error);
    CHECK(errorStatus.statusText == kControlledStopError);
    CHECK(engine.playbackGeneration() == generation);
    CHECK_FALSE(engine.isCurrentPlaybackItem(item.id));
    CHECK(backend->target() == nullptr);

    // The fake's stop, flush, and start all succeed, so these later healthy
    // controls prove Engine does not retract an already delivered terminal
    // error, not actual native SDK stop-failure recoverability.
    backend->clearEvents();
    engine.pause();
    engine.resume();
    CHECK_FALSE(engine.trySeek(item.id, std::chrono::milliseconds{200}));
    auto const settledStatus = engine.status();
    CHECK(settledStatus.transport == Transport::Error);
    CHECK(settledStatus.statusText == kControlledStopError);
    CHECK(backend->events().empty());
    CHECK(failures.size() == 1);
  }

  // Not a native SDK failure test: the fake's stop always succeeds, and the
  // observer only models the terminal error a native backend may report from
  // inside a stop. The assertions prove the old target's error does not fail
  // the replacement, not which retirement or generation check rejects it,
  // nor any native SDK stop-failure behavior.
  TEST_CASE("Engine - backend error reported during replacement stop does not fail the replacement playback",
            "[concurrency]")
  {
    auto workerEntered = std::binary_semaphore{0};
    auto workerRelease = std::binary_semaphore{0};
    auto workerFlushed = std::binary_semaphore{0};
    auto failures = std::vector<Engine::PlaybackFailure>{};
    auto injectedErrorCount = std::atomic<std::size_t>{0};
    auto armed = std::atomic<bool>{true};
    auto const format = makeEngineTestFormat();
    auto const item = makePlaybackItem(PlaybackInput{.filePath = "controlled-stop.flac"});
    auto const replacement = makePlaybackItem(PlaybackInput{.filePath = "replacement.flac"});
    auto backendPtr = std::make_unique<FakeCapturingBackend>();
    auto* const backend = backendPtr.get();
    auto engine = Engine{std::move(backendPtr),
                         makeEngineTestDevice(),
                         makePathScriptedDecoderFactory({{.path = item.input.filePath,
                                                          .info = makeScriptedStreamInfo(format),
                                                          .data = std::vector<std::byte>(4096, std::byte{0x11})},
                                                         {.path = replacement.input.filePath,
                                                          .info = makeScriptedStreamInfo(format),
                                                          .data = std::vector<std::byte>(4096, std::byte{0x22})}})};

    engine.setOnPlaybackFailure([&](Engine::PlaybackFailure const& failure) { failures.push_back(failure); });
    engine.play(item);
    REQUIRE(engine.transport() == Transport::Playing);
    auto const generation = engine.playbackGeneration();

    // Same observer contract as the seek case: recordEvent runs under the fake
    // mutex, so the pre-captured target is invoked directly and the once flag
    // disarms the injection before any later cleanup can touch the pointer.
    auto* const target = backend->target();
    REQUIRE(target != nullptr);
    backend->setEventObserver(
      [target, &armed, &injectedErrorCount](std::string_view name)
      {
        if (name == "stop" && armed.exchange(false, std::memory_order_acq_rel))
        {
          injectedErrorCount.fetch_add(1, std::memory_order_relaxed);
          target->handleBackendError(kControlledStopError);
        }
      });
    backend->clearEvents();

    // Hold the event worker so anything the replacement teardown queues cannot
    // be reduced until after the replacement command returns.
    auto releaseWorker = utility::ScopedRegistration{[&] { workerRelease.release(); }};
    engine.defer(
      [&]
      {
        workerEntered.release();
        workerRelease.acquire();
      });
    REQUIRE(workerEntered.try_acquire_for(std::chrono::seconds{5}));

    // Report an old-target error during replacement teardown. After queued work
    // settles, the replacement must remain current without a playback failure;
    // this does not distinguish rejection at callback admission from reduction.
    engine.play(replacement);

    REQUIRE(engine.transport() == Transport::Playing);
    REQUIRE(engine.isCurrentPlaybackItem(replacement.id));
    CHECK(engine.playbackGeneration() > generation);
    // The replacement published a live target. Its raw address is not compared
    // with the retired one because allocator reuse could make them equal.
    REQUIRE(backend->target() != nullptr);
    REQUIRE(backend->events().size() == 4);
    CHECK(backend->events().at(0).name == "stop");
    CHECK(backend->events().at(1).name == "close");
    CHECK(backend->events().at(2).name == "open");
    CHECK(backend->events().at(3).name == "start");
    REQUIRE(injectedErrorCount.load(std::memory_order_relaxed) == 1);

    // Release the worker and flush the event queue with one more public defer:
    // after earlier queued work settles, no failure was delivered and the
    // replacement remains current.
    releaseWorker.reset();
    engine.defer([&] { workerFlushed.release(); });
    REQUIRE(workerFlushed.try_acquire_for(std::chrono::seconds{5}));

    CHECK(injectedErrorCount.load(std::memory_order_relaxed) == 1);
    CHECK(failures.empty());
    CHECK(engine.transport() == Transport::Playing);
    CHECK(engine.isCurrentPlaybackItem(replacement.id));
    CHECK(engine.playbackGeneration() > generation);
    CHECK(backend->target() != nullptr);
  }
} // namespace ao::audio::test
