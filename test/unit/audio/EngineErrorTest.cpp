// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "EngineTestSupport.h"
#include "FakeCapturingBackend.h"
#include "ScriptedDecoderSession.h"
#include <ao/Error.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/DecodedStreamInfo.h>
#include <ao/audio/Device.h>
#include <ao/audio/Engine.h>
#include <ao/audio/Format.h>
#include <ao/audio/PlaybackInput.h>
#include <ao/audio/Transport.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <expected>
#include <future>
#include <memory>
#include <utility>
#include <vector>

namespace ao::audio::test
{
  namespace
  {
    std::future<Engine::PlaybackFailure> captureNextFailure(Engine& engine)
    {
      auto promisePtr = std::make_shared<std::promise<Engine::PlaybackFailure>>();
      auto future = promisePtr->get_future();

      engine.setOnPlaybackFailure([promisePtr](Engine::PlaybackFailure const& failure)
                                  { promisePtr->set_value(failure); });

      return future;
    }

    Engine::PlaybackFailure requireFailure(std::future<Engine::PlaybackFailure>& future)
    {
      REQUIRE(future.wait_for(std::chrono::seconds{15}) == std::future_status::ready);
      return future.get();
    }
  } // namespace

  TEST_CASE("Engine - play reports decoder and backend setup failures", "[audio][unit][engine][error]")
  {
    auto const device = Device{.id = DeviceId{"test-device"},
                               .displayName = "Test",
                               .description = "Test",
                               .isDefault = false,
                               .backendId = kBackendNone};

    SECTION("Unsupported extension")
    {
      auto engine = Engine{std::make_unique<FakeCapturingBackend>(), device};
      auto const desc = PlaybackInput{.filePath = "song.txt"};
      auto failureFuture = captureNextFailure(engine);

      auto const item = makePlaybackItem(desc);
      engine.play(item);

      auto const failure = requireFailure(failureFuture);
      CHECK(failure.kind == Engine::PlaybackFailureKind::TrackOpen);
      CHECK(failure.itemId == item.id);
      CHECK(failure.input.filePath == desc.filePath);
      CHECK(failure.generation > 0);
      CHECK(failure.recoverable);
      CHECK(failure.error.message.contains("Unsupported audio file extension"));

      CHECK(engine.status().transport == Transport::Error);
      CHECK(engine.status().statusText.contains("Unsupported audio file extension"));
    }

    SECTION("Decoder open failure")
    {
      auto const factory = [](auto const&, auto const& fmt)
      {
        auto decPtr = std::make_unique<ScriptedDecoderSession>(DecodedStreamInfo{
          .sourceFormat = fmt, .outputFormat = fmt, .duration = std::chrono::milliseconds{0}, .isLossy = false});

        decPtr->setOpenResult(std::unexpected(Error{.message = "open failed"}));
        return decPtr;
      };

      auto engine = Engine{std::make_unique<FakeCapturingBackend>(), device, factory};
      auto const desc = PlaybackInput{.filePath = "song.flac"};
      auto failureFuture = captureNextFailure(engine);

      auto const item = makePlaybackItem(desc);
      engine.play(item);

      auto const failure = requireFailure(failureFuture);
      CHECK(failure.kind == Engine::PlaybackFailureKind::TrackOpen);
      CHECK(failure.itemId == item.id);
      CHECK(failure.input.filePath == desc.filePath);
      CHECK(failure.generation > 0);
      CHECK(failure.recoverable);
      CHECK(failure.error.message == "open failed");

      CHECK(engine.status().transport == Transport::Error);
      CHECK(engine.status().statusText == "open failed");
    }

    SECTION("Backend open failure")
    {
      auto backendPtr = std::make_unique<FakeCapturingBackend>();

      backendPtr->setOpenResult(std::unexpected(Error{.message = "hw init failed"}));

      auto const factory = [](auto const&, auto const& fmt)
      {
        auto decPtr = std::make_unique<ScriptedDecoderSession>(DecodedStreamInfo{
          .sourceFormat = fmt,
          .outputFormat = {.sampleRate = 44100, .channels = 2, .bitDepth = 16, .isFloat = false, .isInterleaved = true},
          .duration = std::chrono::milliseconds{0},
          .isLossy = false});

        decPtr->setReadScript({{{}, true}});
        return decPtr;
      };

      auto engine = Engine{std::move(backendPtr), device, factory};
      auto const desc = PlaybackInput{.filePath = "song.flac"};
      auto failureFuture = captureNextFailure(engine);

      auto const item = makePlaybackItem(desc);
      engine.play(item);

      auto const failure = requireFailure(failureFuture);
      CHECK(failure.kind == Engine::PlaybackFailureKind::RouteActivation);
      CHECK(failure.itemId == item.id);
      CHECK(failure.input.filePath == desc.filePath);
      CHECK(failure.generation > 0);
      CHECK_FALSE(failure.recoverable);
      CHECK(failure.error.message == "hw init failed");

      CHECK(engine.status().transport == Transport::Error);
      CHECK(engine.status().statusText == "hw init failed");
    }

    SECTION("Initial offset seek failure")
    {
      auto backendPtr = std::make_unique<FakeCapturingBackend>();
      auto* const backend = backendPtr.get();

      auto const factory = [](auto const&, auto const& fmt)
      {
        auto decPtr = std::make_unique<ScriptedDecoderSession>(DecodedStreamInfo{
          .sourceFormat = fmt,
          .outputFormat = {.sampleRate = 44100, .channels = 2, .bitDepth = 16, .isFloat = false, .isInterleaved = true},
          .duration = std::chrono::seconds{1},
          .isLossy = false});

        decPtr->setReadScript({{{}, true}});
        decPtr->setSeekResult(
          std::unexpected(Error{.code = Error::Code::SeekFailed, .message = "restore seek failed"}));
        return decPtr;
      };

      auto engine = Engine{std::move(backendPtr), device, factory};
      auto const desc = PlaybackInput{.filePath = "song.flac"};
      auto failureFuture = captureNextFailure(engine);

      auto const item = makePlaybackItem(desc);
      engine.play(item, std::chrono::milliseconds{50});

      auto const failure = requireFailure(failureFuture);
      CHECK(failure.kind == Engine::PlaybackFailureKind::RouteActivation);
      CHECK(failure.itemId == item.id);
      CHECK(failure.input.filePath == desc.filePath);
      CHECK(failure.generation > 0);
      CHECK_FALSE(failure.recoverable);
      CHECK(failure.error.code == Error::Code::SeekFailed);
      CHECK(failure.error.message == "restore seek failed");

      auto const snap = engine.status();
      CHECK(snap.transport == Transport::Error);
      CHECK(snap.statusText == "restore seek failed");

      auto const events = backend->events();
      CHECK(
        std::ranges::none_of(events, [](FakeCapturingBackend::Event const& event) { return event.name == "open"; }));
      CHECK(
        std::ranges::none_of(events, [](FakeCapturingBackend::Event const& event) { return event.name == "start"; }));
    }
  }

  TEST_CASE("Engine - source decode error transitions to Error and ends track", "[audio][unit][engine][error]")
  {
    auto const device = Device{.id = DeviceId{"test-device"},
                               .displayName = "Test",
                               .description = "Test",
                               .isDefault = false,
                               .backendId = kBackendNone};
    auto backendPtr = std::make_unique<FakeCapturingBackend>();

    auto const fmt = Format{.sampleRate = 44100, .channels = 2, .bitDepth = 16, .isInterleaved = true};
    auto const factory = [fmt](auto const&, auto const&)
    {
      auto decPtr = std::make_unique<ScriptedDecoderSession>(DecodedStreamInfo{
        .sourceFormat = fmt, .outputFormat = fmt, .duration = std::chrono::seconds{1}, .isLossy = false});

      // First block succeeds (preroll), second block fails
      // 100,000 bytes at 44.1kHz stereo 16-bit is ~566ms, satisfying the 500ms preroll
      decPtr->setReadScript({{.data = std::vector<std::byte>(100000, std::byte{0}), .endOfStream = false},
                             {.endOfStream = false, .result = std::unexpected(Error{.message = "decode failed"})}});
      return decPtr;
    };

    auto engine = Engine{std::move(backendPtr), device, factory};
    auto const desc = PlaybackInput{.filePath = "fail.flac"};
    auto failureFuture = captureNextFailure(engine);

    auto errorPromise = std::promise<void>{};
    auto errorFuture = errorPromise.get_future();
    engine.setOnTrackEnded([&errorPromise](Engine::TrackEnded const&) { errorPromise.set_value(); });

    auto const item = makePlaybackItem(desc);
    engine.play(item);

    // The StreamingSource decode loop runs in a background thread.
    // It should hit the error and call handleSourceError, which now
    // fires onTrackEnded so we can synchronize without polling.
    CHECK(errorFuture.wait_for(std::chrono::seconds{15}) == std::future_status::ready);

    auto const failure = requireFailure(failureFuture);
    CHECK(failure.kind == Engine::PlaybackFailureKind::Decode);
    CHECK(failure.itemId == item.id);
    CHECK(failure.input.filePath == desc.filePath);
    CHECK(failure.generation > 0);
    CHECK(failure.recoverable);
    CHECK(failure.error.message == "decode failed");

    auto const snap = engine.status();
    CHECK(snap.transport == Transport::Error);
    CHECK(snap.statusText == "decode failed");
  }

  TEST_CASE("Engine - backend runtime error reports device failure", "[audio][unit][engine][error]")
  {
    auto const device = makeEngineTestDevice();
    auto backendPtr = std::make_unique<FakeCapturingBackend>();
    auto* backend = backendPtr.get();
    auto engine = Engine{std::move(backendPtr), device, makeScriptedEngineDecoderFactory()};
    auto const desc = PlaybackInput{.filePath = "song.flac"};
    auto failureFuture = captureNextFailure(engine);

    auto const item = makePlaybackItem(desc);
    engine.play(item);
    REQUIRE(backend->target() != nullptr);

    backend->emitBackendError("device disappeared");

    auto const failure = requireFailure(failureFuture);
    CHECK(failure.kind == Engine::PlaybackFailureKind::DeviceLost);
    CHECK(failure.itemId == item.id);
    CHECK(failure.input.filePath == desc.filePath);
    CHECK(failure.generation > 0);
    CHECK_FALSE(failure.recoverable);
    CHECK(failure.error.code == Error::Code::IoError);
    CHECK(failure.error.message == "device disappeared");

    auto const snap = engine.status();
    CHECK(snap.transport == Transport::Error);
    CHECK(snap.statusText == "device disappeared");
  }
} // namespace ao::audio::test
