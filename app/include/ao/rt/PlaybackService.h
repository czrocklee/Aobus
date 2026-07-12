// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "PlaybackFailure.h"
#include "PlaybackState.h"
#include "PreparedPlayback.h"
#include "Subscription.h"
#include "ViewIds.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/PlaybackInput.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace ao::library
{
  class MusicLibrary;
}

namespace ao::async
{
  class Executor;
}

namespace ao::audio
{
  class BackendProvider;
  class Player;
}

namespace ao::rt
{
  class PlaybackSessionPersistence;
  class PlaybackSequenceService;
  class AppRuntime;
  class NotificationService;
  struct PlaybackTransportSessionState;

  class PlaybackService final
  {
  public:
    struct NowPlayingChanged final
    {
      TrackId trackId = kInvalidTrackId;
      ListId sourceListId = kInvalidListId;
      std::optional<PreparedNextToken> optPreparedNextToken{};
    };

    struct QualityChanged final
    {
      QualityState quality{};
      bool ready = false;
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
      NowPlayingInfo item{};
      audio::PlaybackInput input{};
    };

    // PlaybackService takes exclusive ownership of its direct audio collaborator.
    // The composition root configures Player before transferring ownership.
    PlaybackService(async::Executor& executor,
                    library::MusicLibrary& library,
                    NotificationService& notifications,
                    std::unique_ptr<audio::Player> playerPtr);
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
    std::chrono::milliseconds elapsed() const;

    // Subscription registration is part of the executor-affinity contract: these
    // onXxx() methods must be called on the executor's owning thread, and the
    // returned Subscription must likewise be reset on that thread. Handlers are
    // invoked on the executor thread when the matching signal is emitted. A
    // handler must not synchronously destroy this service; Debug contracts
    // require teardown to be deferred to a later executor turn.

    Subscription onPreparing(std::move_only_function<void()> handler);
    Subscription onStarted(std::move_only_function<void()> handler);
    Subscription onPaused(std::move_only_function<void()> handler);
    Subscription onIdle(std::move_only_function<void()> handler);
    Subscription onNowPlayingChanged(std::move_only_function<void(NowPlayingChanged const&)> handler);
    Subscription onOutputDeviceChanged(std::move_only_function<void(OutputDeviceSelection const&)> handler);
    Subscription onStopped(std::move_only_function<void()> handler);
    Subscription onOutputDevicesChanged(std::move_only_function<void()> handler);
    Subscription onQualityChanged(std::move_only_function<void(QualityChanged const&)> handler);
    Subscription onVolumeChanged(std::move_only_function<void(float)> handler);
    Subscription onMutedChanged(std::move_only_function<void(bool)> handler);
    Subscription onRevealTrackRequested(std::move_only_function<void(RevealTrackRequested const&)> handler);
    Subscription onSeekUpdate(std::move_only_function<void(SeekUpdate const&)> handler);
    Subscription onPlaybackFailure(std::move_only_function<void(PlaybackFailure const&)> handler);

    Result<PlaybackStartReceipt> playTrack(TrackId trackId, ListId sourceListId);

    // Lower-level playback entry point: start a fully-resolved request.
    // playTrack() resolves a TrackId via the library and forwards here.
    // Returns a failure when the track cannot be resolved or playback is
    // rejected before the engine starts.
    Result<PreparedPlaybackStart> stagePlayback(PlaybackRequest const& request,
                                                ListId sourceListId,
                                                std::chrono::milliseconds initialOffset = {});
    Result<PlaybackStartReceipt> commitPlayback(PreparedPlaybackStart&& preparedStart);
    Result<PlaybackStartReceipt> play(PlaybackRequest const& request,
                                      ListId sourceListId,
                                      std::chrono::milliseconds initialOffset = {});
    Result<PreparedNextToken> prepareNext(PlaybackRequest const& request, ListId sourceListId);
    Result<PreparedNextToken> prepareNext(TrackId trackId, ListId sourceListId);
    std::optional<PreparedNextToken> clearPreparedNext();

    // Register an audio backend provider. Called by the composition root
    // (via AppRuntime::addAudioProvider) during bootstrap.
    void addProvider(std::unique_ptr<audio::BackendProvider> providerPtr);
    void pause();
    void resume();
    PreparedCancellationBarrier stop();
    void seek(std::chrono::milliseconds elapsed, SeekMode mode = SeekMode::Final);
    void setOutputDevice(audio::BackendId const& backendId,
                         audio::DeviceId const& deviceId,
                         audio::ProfileId const& profileId);
    void setVolume(float volume);
    void setMuted(bool muted);
    void revealPlayingTrack();
    void revealTrack(TrackId trackId, ViewId preferredViewId = kInvalidViewId, ListId preferredListId = kInvalidListId);

  private:
    friend class AppRuntime;
    friend class PlaybackSessionPersistence;
    friend class PlaybackSequenceService;

    struct SequencePreparedNextReceipt final
    {
      PreparedNextToken token{};
      std::uint64_t issuedGeneration = 0;
    };

    void shutdown() noexcept;
    void bindPlaybackFailureRecovery(PlaybackFailureRecoveryHandler handler);
    void unbindPlaybackFailureRecovery();
    bool isPublishingAcceptedStart() const;
    Result<SequencePreparedNextReceipt> prepareNextWithReceipt(PlaybackRequest const& request, ListId sourceListId);
    Result<PlaybackStartReceipt> playSequenceTrack(TrackId trackId, ListId sourceListId);
    Result<SequencePreparedNextReceipt> prepareSequenceNext(TrackId trackId, ListId sourceListId);
    std::optional<PreparedNextToken> clearSequencePreparedNext();
    PreparedCancellationBarrier stopSequence();
    PlaybackTransportSessionState playbackTransportSessionState();
    Result<> restorePlaybackTransport(PlaybackTransportSessionState const& session,
                                      std::move_only_function<void(std::chrono::milliseconds) noexcept> beforePublish);
    void discardPlaybackTransportSnapshot();

    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::rt
