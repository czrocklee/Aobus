// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 RockStudio Contributors

#pragma once

#include "platform/linux/ui/PlaybackBar.h"
#include "platform/linux/ui/TrackPageGraph.h"
#include "platform/linux/ui/GtkMainThreadDispatcher.h"
#include <rs/audio/PlaybackController.h>
#include <rs/audio/PlaybackTypes.h>

#include <gtkmm.h>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace app::ui
{
  /**
   * ActivePlaybackSequence holds the list of tracks currently being played.
   */
  struct ActivePlaybackSequence final
  {
    std::vector<rs::TrackId> trackIds;
    std::size_t currentIndex = 0;
    std::optional<rs::ListId> sourceListId;
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
    virtual TrackPageContext* findTrackPageContext(rs::ListId listId) = 0;
    virtual void showListPage(rs::ListId listId) = 0;
    virtual void updatePlaybackStatus(rs::audio::PlaybackSnapshot const& snapshot) = 0;
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

    void playCurrentSelection();
    void pausePlayback();
    void stopPlayback();
    void seekPlayback(std::uint32_t positionMs);

    bool startPlaybackFromVisiblePage(TrackViewPage const& page, rs::TrackId trackId);
    bool startPlaybackSequence(std::vector<rs::TrackId> trackIds,
                               rs::TrackId startTrackId,
                               std::optional<rs::ListId> sourceListId = std::nullopt);

    void jumpToPlayingList();
    void handlePlaybackFinished();
    void clearActivePlaybackSequence();
    
    void setPlaybackController(std::unique_ptr<rs::audio::PlaybackController> controller);
    rs::audio::PlaybackController* playbackController() { return _playbackController.get(); }

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
    std::unique_ptr<rs::audio::PlaybackController> _playbackController;
    std::uint32_t _playbackTimer = 0;
    std::optional<ActivePlaybackSequence> _activePlaybackSequence;
    rs::audio::TransportState _lastPlaybackState = rs::audio::TransportState::Idle;
    std::string _lastPlaybackErrorMessage;
  };

} // namespace app::ui
