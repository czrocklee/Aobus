// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "GtkMainThreadDispatcher.h"
#include "PlaybackBar.h"
#include "TrackPageGraph.h"
#include <ao/audio/Player.h>
#include <ao/audio/Types.h>

#include <gtkmm.h>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ao::gtk
{
  /**
   * ActivePlaybackSequence holds the list of tracks currently being played.
   */
  struct ActivePlaybackSequence final
  {
    std::vector<ao::TrackId> trackIds;
    std::size_t currentIndex = 0;
    std::optional<ao::ListId> sourceListId;
  };

  /**
   * IPlaybackHost defines the interface that the host window must implement
   * to support playback orchestration.
   */
  class IPlaybackHost
  {
  public:
    virtual ~IPlaybackHost() = default;
    virtual TrackPageContext const* currentVisibleTrackPageContext() const = 0;
    virtual TrackPageContext* findTrackPageContext(ao::ListId listId) = 0;
    virtual void showListPage(ao::ListId listId) = 0;
    virtual void updatePlaybackStatus(ao::audio::Player::Status const& status) = 0;
    virtual void showPlaybackMessage(std::string const& message,
                                     std::optional<std::chrono::seconds> timeout = std::nullopt) = 0;
  };

  /**
   * PlaybackCoordinator manages playback state, transport, and UI synchronization.
   */
  class PlaybackCoordinator final
  {
  public:
    PlaybackCoordinator(IPlaybackHost& host,
                        std::shared_ptr<GtkMainThreadDispatcher> dispatcher,
                        std::function<TrackRowDataProvider*()> providerSource);
    ~PlaybackCoordinator();

    PlaybackBar& playbackBar() { return *_playbackBar; }

    using OutputChangedSignal = sigc::signal<void(ao::audio::BackendId, ao::audio::DeviceId, ao::audio::ProfileId)>;
    OutputChangedSignal& signalOutputChanged() { return _playbackBar->signalOutputChanged(); }

    void playCurrentSelection();
    void pausePlayback();
    void stopPlayback();
    void seekPlayback(std::uint32_t positionMs);

    bool startPlaybackFromVisiblePage(TrackViewPage const& page, ao::TrackId trackId);
    bool startPlaybackSequence(std::vector<ao::TrackId> trackIds,
                               ao::TrackId startTrackId,
                               std::optional<ao::ListId> sourceListId = std::nullopt);

    void jumpToPlayingList();
    void handlePlaybackFinished();
    void clearActivePlaybackSequence();

    void setPlaybackController(std::unique_ptr<ao::audio::Player> controller);
    ao::audio::Player* player() { return _player.get(); }

  private:
    void setupPlayback();
    void refreshPlaybackBar();

    void onPlayRequested();
    void onPauseRequested();
    void onStopRequested();
    void onSeekRequested(std::uint32_t positionMs);

    bool playTrackAtSequenceIndex(std::size_t index);

    IPlaybackHost& _host;
    std::function<TrackRowDataProvider*()> _providerSource;
    std::unique_ptr<PlaybackBar> _playbackBar;
    std::shared_ptr<GtkMainThreadDispatcher> _dispatcher;
    std::unique_ptr<ao::audio::Player> _player;
    std::uint32_t _playbackTimer = 0;
    std::optional<ActivePlaybackSequence> _activePlaybackSequence;
    ao::audio::Transport _lastPlaybackState = ao::audio::Transport::Idle;
    std::string _lastPlaybackErrorMessage;
  };
} // namespace ao::gtk
