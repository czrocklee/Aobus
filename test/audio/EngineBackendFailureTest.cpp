// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "EngineFailureTestSupport.h"
#include "EngineTestSupport.h"
#include "FakeCapturingBackend.h"
#include <ao/Error.h>
#include <ao/audio/Backend.h>
#include <ao/audio/Engine.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/PlaybackInput.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/Transport.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <expected>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::audio::test
{
  namespace
  {
    DecoderFactoryFn makeRefusalDecoderFactory()
    {
      return makePathScriptedDecoderFactory({{.path = "song.flac",
                                              .info = makeScriptedStreamInfo(makeEngineTestFormat()),
                                              .data = std::vector<std::byte>(4096, std::byte{0})}});
    }

    std::size_t countEvents(std::vector<FakeCapturingBackend::Event> const& events, std::string_view name)
    {
      return static_cast<std::size_t>(
        std::ranges::count_if(events, [name](FakeCapturingBackend::Event const& event) { return event.name == name; }));
    }
  } // namespace

  TEST_CASE("Engine - identical open errors with opposite causes produce different failure kinds")
  {
    auto const error = Error{.code = Error::Code::FormatRejected, .message = "backend refused the requested format"};

    auto const runRefusedPlay = [&error](BackendOpenFailure::Cause cause) -> Engine::PlaybackFailure
    {
      auto backendPtr = std::make_unique<FakeCapturingBackend>();
      backendPtr->setOpenResult(std::unexpected{error}, cause);
      auto engine = Engine{std::move(backendPtr), makeEngineTestDevice(), makeRefusalDecoderFactory()};
      auto failureFuture = captureNextFailure(engine);

      engine.play(makePlaybackItem(PlaybackInput{.filePath = "song.flac"}));

      return requireFailure(failureFuture);
    };

    auto const outputFailure = runRefusedPlay(BackendOpenFailure::Cause::OutputFailure);
    CHECK(outputFailure.kind == Engine::PlaybackFailureKind::RouteActivation);
    CHECK_FALSE(outputFailure.recoverable);

    auto const trackFailure = runRefusedPlay(BackendOpenFailure::Cause::TrackRouteIncompatible);
    CHECK(trackFailure.kind == Engine::PlaybackFailureKind::TrackOpen);
    CHECK(trackFailure.recoverable);

    // Neither the code nor the message decides the classification: the original
    // error is identical in both notifications.
    CHECK(outputFailure.error.code == error.code);
    CHECK(outputFailure.error.message == error.message);
    CHECK(trackFailure.error.code == error.code);
    CHECK(trackFailure.error.message == error.message);
  }

  TEST_CASE("Engine - route-incompatible refusal preserves the original error and track context")
  {
    auto backendPtr = std::make_unique<FakeCapturingBackend>();
    auto* const backend = backendPtr.get();
    auto const injected = Error{.code = Error::Code::FormatRejected, .message = "route cannot carry this source"};
    backendPtr->setOpenResult(std::unexpected{injected}, BackendOpenFailure::Cause::TrackRouteIncompatible);

    auto engine = Engine{std::move(backendPtr), makeEngineTestDevice(), makeRefusalDecoderFactory()};
    auto const desc = PlaybackInput{.filePath = "song.flac"};
    auto failureFuture = captureNextFailure(engine);

    auto const item = makePlaybackItem(desc);
    engine.play(item);

    auto const failure = requireFailure(failureFuture);
    CHECK(failure.kind == Engine::PlaybackFailureKind::TrackOpen);
    CHECK(failure.recoverable);
    CHECK(failure.error.code == injected.code);
    CHECK(failure.error.message == injected.message);
    // The original error location is preserved, not re-attributed to Engine code.
    CHECK(std::string_view{failure.error.location.file_name()} == std::string_view{injected.location.file_name()});
    CHECK(failure.error.location.line() == injected.location.line());
    CHECK(failure.itemId == item.id);
    CHECK(failure.input.filePath == desc.filePath);
    CHECK(failure.generation > 0);
    CHECK(failure.generation == engine.playbackGeneration());

    // The failure came from exactly one attempted backend open, not from track inspection.
    auto const events = backend->events();
    CHECK(countEvents(events, "open-failed") == 1U);
    CHECK(countEvents(events, "open") == 0U);
    CHECK(countEvents(events, "start") == 0U);
  }

  TEST_CASE("Engine - refused open neither falls back nor starts the route")
  {
    auto backendPtr = std::make_unique<FakeCapturingBackend>();
    auto* const backend = backendPtr.get();
    backend->setOpenResult(
      std::unexpected{Error{.code = Error::Code::FormatRejected, .message = "route cannot carry this source"}},
      BackendOpenFailure::Cause::TrackRouteIncompatible);

    auto engine = Engine{std::move(backendPtr), makeEngineTestDevice(), makeRefusalDecoderFactory()};
    auto failureFuture = captureNextFailure(engine);

    engine.play(makePlaybackItem(PlaybackInput{.filePath = "song.flac"}));

    auto const failure = requireFailure(failureFuture);
    CHECK(failure.kind == Engine::PlaybackFailureKind::TrackOpen);

    auto const events = backend->events();
    CHECK(countEvents(events, "open-failed") == 1U);
    CHECK(countEvents(events, "open") == 0U);
    CHECK(countEvents(events, "start") == 0U);
    CHECK(countEvents(events, "close") >= 1U);
    CHECK(backend->target() == nullptr);

    auto const snap = engine.status();
    CHECK(snap.transport == Transport::Error);
    CHECK(snap.statusText == "route cannot carry this source");
  }

  TEST_CASE("Engine - malformed successful backend mode stays terminal")
  {
    auto backendPtr = std::make_unique<FakeCapturingBackend>();
    auto* const backend = backendPtr.get();
    // open() succeeds but confirms a 16-bit endpoint for a 24-bit source.
    backend->setConfirmedEndpointPrecision(16);

    auto const sourceFormat =
      PcmFormat{.sampleRate = 48000, .channels = 2, .encoding = SampleEncoding::Signed24PackedLe};
    auto engine = Engine{std::move(backendPtr),
                         makeEngineTestDevice(),
                         makePathScriptedDecoderFactory({
                           {.path = "24-bit.flac",
                            .info = makeScriptedStreamInfo(sourceFormat),
                            .data = std::vector<std::byte>(4096, std::byte{0})},
                         })};
    auto failureFuture = captureNextFailure(engine);

    engine.play(makePlaybackItem(PlaybackInput{.filePath = "24-bit.flac"}));

    auto const failure = requireFailure(failureFuture);
    CHECK(failure.kind == Engine::PlaybackFailureKind::RouteActivation);
    CHECK_FALSE(failure.recoverable);
    CHECK(failure.error.code == Error::Code::FormatRejected);
    CHECK(failure.error.message == "Backend confirmed a 16-bit endpoint for a 24-bit source");

    auto const snap = engine.status();
    CHECK(snap.transport == Transport::Error);
    CHECK(snap.statusText == failure.error.message);
    CHECK(backend->target() == nullptr);

    auto const events = backend->events();
    CHECK(countEvents(events, "open") == 1U);
    CHECK(countEvents(events, "start") == 0U);
    CHECK(countEvents(events, "close") >= 1U);
  }
} // namespace ao::audio::test
