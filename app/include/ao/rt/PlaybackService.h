// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "CorePrimitives.h"
#include "StateTypes.h"
#include <ao/Type.h>
#include <ao/audio/Backend.h>
#include <ao/audio/Types.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace ao::library
{
  class MusicLibrary;
}

namespace ao::async
{
  class IExecutor;
}

namespace ao::audio
{
  class IBackendProvider;
}

namespace ao::rt
{
  class ViewService;

  class PlaybackService final
  {
  public:
    struct NowPlayingChanged final
    {
      TrackId trackId = kInvalidTrackId;
      ListId sourceListId = kInvalidListId;
    };

    struct QualityChanged final
    {
      audio::Quality quality = audio::Quality::Unknown;
      bool ready = false;
    };

    struct ShuffleModeChanged final
    {
      ShuffleMode mode = ShuffleMode::Off;
    };

    struct RepeatModeChanged final
    {
      RepeatMode mode = RepeatMode::Off;
    };

    enum class SeekMode : std::uint8_t
    {
      Final,
      Preview
    };

    struct SeekUpdate final
    {
      std::chrono::milliseconds elapsed{0};
      SeekMode mode = SeekMode::Final;
    };

    struct RevealTrackRequested final
    {
      TrackId trackId = kInvalidTrackId;
      ListId preferredListId = kInvalidListId;
      ViewId preferredViewId = kInvalidViewId;
    };

    struct PlaybackRequest final
    {
      TrackId trackId = kInvalidTrackId;
      audio::PlaybackInput input{};
      std::string title{};
      std::string artist{};
    };

    PlaybackService(async::IExecutor& executor, ViewService& views, library::MusicLibrary& library);
    ~PlaybackService();

    PlaybackService(PlaybackService const&) = delete;
    PlaybackService& operator=(PlaybackService const&) = delete;
    PlaybackService(PlaybackService&&) = delete;
    PlaybackService& operator=(PlaybackService&&) = delete;

    // Returns the latest published state. Control methods refresh this snapshot
    // synchronously before they return, so after a control call returns state()
    // already reflects that command's result. Only the asynchronous Player
    // callbacks (backend/source events) advance the snapshot on a later executor
    // turn. Like every public method below, must be called on the executor's
    // owning thread (see the affinity note above the subscription methods).
    PlaybackState const& state() const;

    // Subscription registration is part of the executor-affinity contract: these
    // onXxx() methods must be called on the executor's owning thread, and the
    // returned Subscription must likewise be reset on that thread. Handlers are
    // invoked on the executor thread when the matching signal is emitted.

    Subscription onPreparing(std::move_only_function<void()> handler);
    Subscription onStarted(std::move_only_function<void()> handler);
    Subscription onPaused(std::move_only_function<void()> handler);
    Subscription onIdle(std::move_only_function<void()> handler);
    Subscription onNowPlayingChanged(std::move_only_function<void(NowPlayingChanged const&)> handler);
    Subscription onOutputChanged(std::move_only_function<void(OutputSelection const&)> handler);
    Subscription onStopped(std::move_only_function<void()> handler);
    Subscription onDevicesChanged(std::move_only_function<void()> handler);
    Subscription onQualityChanged(std::move_only_function<void(QualityChanged const&)> handler);
    Subscription onVolumeChanged(std::move_only_function<void(float)> handler);
    Subscription onMutedChanged(std::move_only_function<void(bool)> handler);
    Subscription onRevealTrackRequested(std::move_only_function<void(RevealTrackRequested const&)> handler);
    Subscription onSeekUpdate(std::move_only_function<void(SeekUpdate const&)> handler);
    Subscription onShuffleModeChanged(std::move_only_function<void(ShuffleModeChanged const&)> handler);
    Subscription onRepeatModeChanged(std::move_only_function<void(RepeatModeChanged const&)> handler);

    bool playTrack(TrackId trackId, ListId sourceListId);
    TrackId playSelectionInView(ViewId viewId);

    // Lower-level playback entry point: start a fully-resolved request.
    // playTrack() resolves a TrackId via the library and forwards here.
    void play(PlaybackRequest const& request, ListId sourceListId);

    // Register an audio backend provider. Called by the composition root
    // (via AppRuntime::addAudioProvider) during bootstrap.
    void addProvider(std::unique_ptr<audio::IBackendProvider> providerPtr);
    void pause();
    void resume();
    void stop();
    void setShuffleMode(ShuffleMode mode);
    void setRepeatMode(RepeatMode mode);
    void seek(std::chrono::milliseconds elapsed, SeekMode mode = SeekMode::Final);
    void setOutput(audio::BackendId const& backendId,
                   audio::DeviceId const& deviceId,
                   audio::ProfileId const& profileId);
    void setVolume(float volume);
    void setMuted(bool muted);
    void revealPlayingTrack();
    void revealTrack(TrackId trackId, ViewId preferredViewId = kInvalidViewId, ListId preferredListId = kInvalidListId);

  private:
    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::rt
