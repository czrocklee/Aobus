// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "CorePrimitives.h"
#include "StateTypes.h"
#include <ao/audio/IBackendProvider.h>
#include <memory>

namespace ao::library
{
  class MusicLibrary;
}

namespace ao::app
{
  class EventBus;
  class IControlExecutor;
  class ViewService;

  class PlaybackService final
  {
  public:
    PlaybackService(EventBus& events,
                    IControlExecutor& executor,
                    ViewService& views,
                    ao::library::MusicLibrary& library);
    ~PlaybackService();

    PlaybackService(PlaybackService const&) = delete;
    PlaybackService& operator=(PlaybackService const&) = delete;
    PlaybackService(PlaybackService&&) = delete;
    PlaybackService& operator=(PlaybackService&&) = delete;

    PlaybackState state() const;

    void play(ao::audio::TrackPlaybackDescriptor const& descriptor, ao::ListId sourceListId);
    ao::TrackId playSelectionInView(ViewId viewId);
    void pause();
    void resume();
    void stop();
    void seek(std::uint32_t positionMs);
    void setOutput(ao::audio::BackendId const& backendId,
                   ao::audio::DeviceId const& deviceId,
                   ao::audio::ProfileId const& profileId);
    void setVolume(float volume);
    void setMuted(bool muted);
    void revealPlayingTrack();

    void addProvider(std::unique_ptr<ao::audio::IBackendProvider> provider);

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
  };
}
