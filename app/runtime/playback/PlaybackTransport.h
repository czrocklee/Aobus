// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Subscription.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/PlaybackInput.h>
#include <ao/rt/PlaybackFailure.h>
#include <ao/rt/PlaybackState.h>
#include <ao/rt/PreparedPlayback.h>
#include <ao/rt/ViewIds.h>

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
  class PlaybackBootstrap;
  class PlaybackSessionPersistence;
  class PlaybackSuccession;
  class NotificationService;
  struct PlaybackTransportSessionState;

  /** Move-only, non-published explicit playback candidate. */
  class PreparedPlaybackStart final
  {
  public:
    ~PreparedPlaybackStart();

    PreparedPlaybackStart(PreparedPlaybackStart const&) = delete;
    PreparedPlaybackStart& operator=(PreparedPlaybackStart const&) = delete;
    PreparedPlaybackStart(PreparedPlaybackStart&&) noexcept;
    PreparedPlaybackStart& operator=(PreparedPlaybackStart&&) noexcept;

  private:
    struct Impl;

    explicit PreparedPlaybackStart(std::unique_ptr<Impl> implPtr);

    std::unique_ptr<Impl> _implPtr;

    friend class PlaybackTransport;
  };

  class PlaybackTransport final
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

    // PlaybackTransport takes exclusive ownership of its direct audio collaborator.
    // The composition root configures Player before transferring ownership.
    PlaybackTransport(async::Executor& executor,
                      library::MusicLibrary const& library,
                      NotificationService& notifications,
                      std::unique_ptr<audio::Player> playerPtr);
    ~PlaybackTransport();

    PlaybackTransport(PlaybackTransport const&) = delete;
    PlaybackTransport& operator=(PlaybackTransport const&) = delete;
    PlaybackTransport(PlaybackTransport&&) = delete;
    PlaybackTransport& operator=(PlaybackTransport&&) = delete;

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
    // returned subscription must likewise be reset on that thread. Handlers are
    // invoked on the executor thread when the matching signal is emitted. A
    // handler must not synchronously destroy this transport; Debug contracts
    // require teardown to be deferred to a later executor turn.

    async::Subscription onPreparing(std::move_only_function<void()> handler);
    async::Subscription onStarted(std::move_only_function<void()> handler);
    async::Subscription onPaused(std::move_only_function<void()> handler);
    async::Subscription onIdle(std::move_only_function<void()> handler);
    async::Subscription onNowPlayingChanged(std::move_only_function<void(NowPlayingChanged const&)> handler);
    async::Subscription onOutputDeviceChanged(std::move_only_function<void(OutputDeviceSelection const&)> handler);
    async::Subscription onStopped(std::move_only_function<void()> handler);
    async::Subscription onOutputDevicesChanged(std::move_only_function<void()> handler);
    async::Subscription onQualityChanged(std::move_only_function<void(QualityChanged const&)> handler);
    async::Subscription onVolumeChanged(std::move_only_function<void(float)> handler);
    async::Subscription onMutedChanged(std::move_only_function<void(bool)> handler);
    async::Subscription onRevealTrackRequested(std::move_only_function<void(RevealTrackRequested const&)> handler);
    async::Subscription onSeekUpdate(std::move_only_function<void(SeekUpdate const&)> handler);
    async::Subscription onPlaybackFailure(std::move_only_function<void(PlaybackFailure const&)> handler);

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
    friend class PlaybackBootstrap;
    friend class PlaybackSessionPersistence;
    friend class PlaybackSuccession;

    struct SuccessionPreparedNextReceipt final
    {
      PreparedNextToken token{};
      std::uint64_t issuedGeneration = 0;
    };

    void shutdown() noexcept;
    void addProvider(std::unique_ptr<audio::BackendProvider> providerPtr);
    void bindPlaybackFailureRecovery(PlaybackFailureRecoveryHandler handler);
    void unbindPlaybackFailureRecovery();
    bool isPublishingAcceptedStart() const;
    Result<SuccessionPreparedNextReceipt> prepareNextWithReceipt(PlaybackRequest const& request, ListId sourceListId);
    Result<PlaybackStartReceipt> playSuccessionTrack(TrackId trackId, ListId sourceListId);
    Result<SuccessionPreparedNextReceipt> prepareSuccessionNext(TrackId trackId, ListId sourceListId);
    std::optional<PreparedNextToken> clearSuccessionPreparedNext();
    PreparedCancellationBarrier stopSuccession();
    PlaybackTransportSessionState playbackTransportSessionState();
    Result<> restorePlaybackTransport(PlaybackTransportSessionState const& session,
                                      std::move_only_function<void(std::chrono::milliseconds) noexcept> beforePublish);
    void discardPlaybackTransportSnapshot();

    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::rt
