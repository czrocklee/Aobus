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

namespace ao::rt
{
  class IControlExecutor;
  class ViewService;

  class PlaybackService final
  {
  public:
    struct NowPlayingChanged final
    {
      TrackId trackId{};
      ListId sourceListId{};
    };

    struct QualityChanged final
    {
      audio::Quality quality = audio::Quality::Unknown;
      bool ready = false;
    };

    struct RevealTrackRequested final
    {
      TrackId trackId{};
      ListId preferredListId{};
      ViewId preferredViewId{};
    };

    PlaybackService(IControlExecutor& executor, ViewService& views, library::MusicLibrary& library);
    ~PlaybackService();

    PlaybackService(PlaybackService const&) = delete;
    PlaybackService& operator=(PlaybackService const&) = delete;
    PlaybackService(PlaybackService&&) = delete;
    PlaybackService& operator=(PlaybackService&&) = delete;

    PlaybackState const& state() const;

    Subscription onPreparing(std::move_only_function<void()> handler);
    Subscription onStarted(std::move_only_function<void()> handler);
    Subscription onPaused(std::move_only_function<void()> handler);
    Subscription onIdle(std::move_only_function<void()> handler);
    Subscription onNowPlayingChanged(std::move_only_function<void(NowPlayingChanged const&)> handler);
    Subscription onOutputChanged(std::move_only_function<void(OutputSelection const&)> handler);
    Subscription onStopped(std::move_only_function<void()> handler);
    Subscription onDevicesChanged(std::move_only_function<void()> handler);
    Subscription onQualityChanged(std::move_only_function<void(QualityChanged const&)> handler);
    Subscription onRevealTrackRequested(std::move_only_function<void(RevealTrackRequested const&)> handler);

    void play(audio::TrackPlaybackDescriptor const& descriptor, ListId sourceListId);
    TrackId playSelectionInView(ViewId viewId);
    void pause();
    void resume();
    void stop();
    void seek(std::uint32_t positionMs);
    void setOutput(audio::BackendId const& backendId,
                   audio::DeviceId const& deviceId,
                   audio::ProfileId const& profileId);
    void setVolume(float volume);
    void setMuted(bool muted);
    void revealPlayingTrack();

    void addProvider(std::unique_ptr<audio::IBackendProvider> provider);

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
  };
}
