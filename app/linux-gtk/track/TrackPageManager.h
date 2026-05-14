// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "library_io/PlaylistExporter.h"
#include "track/TrackListAdapter.h"
#include "track/TrackPresentation.h"
#include "track/TrackPresentationStore.h"
#include "track/TrackViewPage.h"
#include <runtime/AppSession.h>
#include <runtime/CorePrimitives.h>
#include <runtime/PlaybackService.h>

#include <ao/library/ListView.h>
#include <gtkmm.h>
#include <runtime/TrackSource.h>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace ao::gtk
{
  class ListSidebarController;
  class PlaybackSequenceController;
  class TagEditController;
  class TrackRowCache;

  /**
   * TrackPageContext holds the per-page state for a track list.
   */
  struct TrackPageContext final
  {
    rt::ViewId viewId{};
    std::unique_ptr<TrackListAdapter> adapter = {};
    std::unique_ptr<TrackViewPage> page = {};
    std::unique_ptr<PlaylistExporter> exporter = {};
  };

  /**
   * TrackPageManager manages the creation, lookup, and lifecycle of track pages.
   */
  class TrackPageManager final
  {
  public:
    TrackPageManager(Gtk::Stack& stack,
                     TrackColumnLayoutModel& layoutModel,
                     rt::AppSession& session,
                     PlaybackSequenceController* sequenceController,
                     TagEditController& tagEditController,
                     ListSidebarController& listSidebar,
                     TrackPresentationStore& presentationStore);
    ~TrackPageManager();

    void setPlaybackSequenceController(PlaybackSequenceController& c) { _playbackSequenceController = &c; }

    Gtk::Stack& stack() { return _stack; }

    void clear();
    void rebuild(TrackRowCache& dataProvider, lmdb::ReadTransaction const& txn);

    TrackPageContext* find(rt::ViewId viewId);
    TrackPageContext const* find(rt::ViewId viewId) const;

    TrackPageContext* currentVisible();
    TrackPageContext const* currentVisible() const;

    void setPlayingTrack(std::optional<TrackId> trackId);

    /**
     * @return The list ID of the currently visible page, or allTracksListId() if none.
     */
    ListId activeListId() const;

  private:
    void ensureViewPage(rt::ViewId viewId, TrackRowCache& dataProvider);
    void bindTrackPage(TrackPageContext& ctx);
    void syncLayout();
    void handleRevealTrack(rt::PlaybackService::RevealTrackRequested const& ev);

    Gtk::Stack& _stack;
    TrackColumnLayoutModel& _layoutModel;
    rt::AppSession& _session;
    PlaybackSequenceController* _playbackSequenceController;
    TagEditController& _tagEditController;
    ListSidebarController& _listSidebar;
    TrackPresentationStore& _presentationStore;
    rt::Subscription _revealSub;
    rt::Subscription _nowPlayingSub;
    rt::Subscription _focusSub;
    rt::Subscription _viewDestroyedSub;
    rt::Subscription _projectionChangedSub;

    std::map<rt::ViewId, TrackPageContext> _trackPages;
    std::optional<TrackId> _optPlayingTrackId;
    TrackRowCache* _activeDataProvider = nullptr;
  };
} // namespace ao::gtk
