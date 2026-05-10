// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Type.h>
#include <runtime/CorePrimitives.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace ao::app
{
  class AppSession;
}

namespace ao::gtk
{
  struct ActivePlaybackSequence final
  {
    std::vector<ao::TrackId> trackIds;
    std::size_t currentIndex = 0;
    std::optional<ao::ListId> optSourceListId;
  };

  class TrackRowDataProvider;
  class TrackViewPage;

  class PlaybackController final
  {
  public:
    PlaybackController(ao::app::AppSession& session, TrackRowDataProvider& dataProvider);
    ~PlaybackController();

    PlaybackController(PlaybackController const&) = delete;
    PlaybackController& operator=(PlaybackController const&) = delete;
    PlaybackController(PlaybackController&&) = delete;
    PlaybackController& operator=(PlaybackController&&) = delete;

    // Start playback from a track view page.
    // Builds the sequence from visible tracks, resolves the descriptor for
    // startTrackId, dispatches PlayTrack, and subscribes to transport events.
    bool playFromPage(TrackViewPage& page, ao::TrackId startTrackId);

    // Resume playback from a paused state.
    void resume();

    // Current playback state queries (used by status bar, navigation).
    bool isActive() const;
    std::optional<ao::TrackId> nowPlayingTrackId() const;
    std::optional<ao::ListId> sourceListId() const;

  private:
    void clear();
    void advanceToNext();
    void subscribeEvents();
    void unsubscribeEvents();

    ao::app::AppSession& _session;
    TrackRowDataProvider& _dataProvider;
    std::unique_ptr<ActivePlaybackSequence> _sequence;
    ao::app::Subscription _transportSub;
    ao::app::Subscription _stoppedSub;
  };
}
