// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "runtime/playback/PlaybackBootstrap.h"
#include "runtime/playback/PlaybackTransport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/async/Runtime.h>
#include <ao/audio/BackendProvider.h>
#include <ao/audio/Engine.h>
#include <ao/audio/Player.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/ViewIds.h>

#include <fakeit.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace ao::audio
{
  class NullBackend;
  class RenderTarget;

  namespace test
  {
    template<typename T>
    class SpyBackend;
  }
} // namespace ao::audio

namespace ao::rt::test
{
  using PlaybackFixtureSpyBackend = audio::test::SpyBackend<audio::NullBackend>;

  PlaybackTransport::PlaybackRequest playbackRequest(TrackId trackId,
                                                     std::string_view filePath,
                                                     std::string title,
                                                     std::string artist,
                                                     std::chrono::milliseconds duration,
                                                     std::string album = {},
                                                     ResourceId coverArtId = kInvalidResourceId,
                                                     ViewId sourceViewId = kInvalidViewId);

  // Canonical single-backend, single-device provider status shared by every
  // fixture instance below: "mock_backend" exposes one default "mock_device"
  // and the shared profile.
  audio::BackendProvider::Status makeMockProviderStatus();

  namespace detail
  {
    std::shared_ptr<PlaybackFixtureSpyBackend> makePlaybackFixtureSpyBackend();

    std::unique_ptr<audio::Player> makePlaybackFixturePlayer(async::Runtime& runtime,
                                                             audio::DecoderFactoryFn decoderFactory);

    void configurePlaybackTransportFixture(std::shared_ptr<PlaybackFixtureSpyBackend>& spyBackendPtr,
                                           fakeit::Mock<audio::BackendProvider>& mockProvider,
                                           audio::BackendProvider::Status& status,
                                           audio::BackendProvider::OnDevicesChangedCallback& onDevicesChangedCb,
                                           audio::BackendProvider::OnGraphChangedCallback& onGraphChangedCb,
                                           audio::RenderTarget*& renderTarget,
                                           PlaybackTransport& playbackTransport);
  } // namespace detail

  using rt::test::QueuedExecutor;

  // Shared wiring for the PlaybackTransport tests: a music library, a spy backend,
  // and a mocked BackendProvider that hands out that backend.
  // ExecutorT selects the dispatch model (InlineExecutor runs inline; QueuedExecutor
  // defers until drain()). The provider's devices/graph callbacks and the render
  // target are captured into public members so a test can drive them.
  //
  // The constructor wires the mocks and registers the provider, but it does NOT
  // notify devices: each test triggers onDevicesChangedCb itself because the call
  // sites need different priming (auto-select-and-edge-cases, a single
  // notify-then-drain, or no notify at all to exercise ensureReady()).
  template<typename ExecutorT>
  struct PlaybackTransportFixture final
  {
    explicit PlaybackTransportFixture(audio::DecoderFactoryFn decoderFactory = {})
      : playbackTransport{executor,
                          libraryFixture.library(),
                          notificationService,
                          detail::makePlaybackFixturePlayer(asyncRuntime, std::move(decoderFactory))}
    {
      detail::configurePlaybackTransportFixture(
        spyBackendPtr, mockProvider, status, onDevicesChangedCb, onGraphChangedCb, renderTarget, playbackTransport);
    }

    PlaybackTransportFixture(PlaybackTransportFixture const&) = delete;
    PlaybackTransportFixture& operator=(PlaybackTransportFixture const&) = delete;
    PlaybackTransportFixture(PlaybackTransportFixture&&) = delete;
    PlaybackTransportFixture& operator=(PlaybackTransportFixture&&) = delete;
    ~PlaybackTransportFixture() = default;

    std::string installAudioFixture(std::string_view const fileName = "basic_metadata.flac",
                                    std::string_view const libraryUri = "playable.flac")
    {
      return audio::test::installAudioFixture(libraryFixture.root(), fileName, libraryUri);
    }

    // Declaration order matters: the executor and async runtime must outlive
    // NotificationService and PlaybackTransport, and playbackTransport (destroyed
    // first) tears down its Player while the provider mock is still alive.
    MusicLibraryFixture libraryFixture;
    ExecutorT executor;
    async::Runtime asyncRuntime{executor, 1};
    NotificationService notificationService{asyncRuntime};

    std::shared_ptr<PlaybackFixtureSpyBackend> spyBackendPtr = detail::makePlaybackFixtureSpyBackend();
    fakeit::Mock<audio::BackendProvider> mockProvider;
    audio::BackendProvider::Status status = makeMockProviderStatus();

    audio::BackendProvider::OnDevicesChangedCallback onDevicesChangedCb;
    audio::BackendProvider::OnGraphChangedCallback onGraphChangedCb;
    audio::RenderTarget* renderTarget = nullptr;

    PlaybackTransport playbackTransport;
  };
} // namespace ao::rt::test
