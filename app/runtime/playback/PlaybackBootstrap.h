// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <memory>

namespace ao::async
{
  class Executor;
}

namespace ao::audio
{
  class BackendProvider;
}

namespace ao::library
{
  class MusicLibrary;
}

namespace ao::rt
{
  class LibraryChanges;
  class PlaybackService;
  class PlaybackSuccession;
  class PlaybackTransport;

  /** Composition-only construction, provider registration, and shutdown role. */
  class PlaybackBootstrap final
  {
  public:
    explicit PlaybackBootstrap(PlaybackTransport& transport) noexcept;

    PlaybackService createPlaybackService(async::Executor& executor,
                                          PlaybackSuccession& succession,
                                          library::MusicLibrary const& library,
                                          LibraryChanges const& changes);
    void addProvider(std::unique_ptr<audio::BackendProvider> providerPtr);
    void shutdown() noexcept;

  private:
    PlaybackTransport& _transport;
  };
} // namespace ao::rt
