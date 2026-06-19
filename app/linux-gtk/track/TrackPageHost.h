// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "image/ImageCache.h"
#include "image/ThumbnailLoader.h"
#include "layout/LayoutConstants.h"
#include "track/TrackViewPage.h"
#include <ao/Type.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/PlaybackService.h>
#include <ao/uimodel/track/TrackPresentationViewModel.h>

#include <map>
#include <memory>

namespace ao::lmdb
{
  class ReadTransaction;
}

namespace ao::uimodel::playback
{
  class PlaybackQueueModel;
}
namespace Gtk
{
  class Stack;
}

namespace ao::gtk
{
  class ListNavigationController;
  class TagEditController;
  class TrackRowCache;
  class TrackListModel;
  class TrackViewPage;

  /**
   * TrackPageContext holds the per-page state for a track list.
   */
  struct TrackPageContext final
  {
    rt::ViewId viewId{};
    Glib::RefPtr<TrackListModel> modelPtr = {};
    std::unique_ptr<TrackViewPage> pagePtr = {};
  };

  /**
   * TrackPageHost manages the creation, lookup, and lifecycle of track pages.
   */
  class TrackPageHost final
  {
  public:
    TrackPageHost(Gtk::Stack& stack,
                  rt::AppRuntime& runtime,
                  uimodel::playback::PlaybackQueueModel* queueModel,
                  TagEditController& tagEditController,
                  ListNavigationController& listNavigation,
                  uimodel::track::TrackPresentationViewModel& presentationStore);
    ~TrackPageHost();

    // Not copyable or movable
    TrackPageHost(TrackPageHost const&) = delete;
    TrackPageHost& operator=(TrackPageHost const&) = delete;
    TrackPageHost(TrackPageHost&&) = delete;
    TrackPageHost& operator=(TrackPageHost&&) = delete;

    void setPlaybackQueueModel(uimodel::playback::PlaybackQueueModel& controller) { _playbackQueueModel = &controller; }

    Gtk::Stack& stack() { return _stack; }
    uimodel::track::TrackPresentationViewModel& presentationStore() { return _presentationStore; }

    void clear();
    void rebuild(TrackRowCache& dataProvider, lmdb::ReadTransaction const& txn);

    TrackPageContext* find(rt::ViewId viewId);
    TrackPageContext const* find(rt::ViewId viewId) const;

    TrackPageContext* currentVisible();
    TrackPageContext const* currentVisible() const;

    void setPlayingTrack(TrackId trackId);

    /**
     * @return The list ID of the currently visible page, or rt::kAllTracksListId if none.
     */
    ListId activeListId() const;

  private:
    void ensureViewPage(rt::ViewId viewId, TrackRowCache& dataProvider);
    void bindTrackPage(TrackPageContext& ctx);
    void syncLayout();
    void handleRevealTrack(rt::PlaybackService::RevealTrackRequested const& ev);
    rt::ViewId tryFindViewByPreferredList(ListId preferredListId);
    void tryRevealTrackInView(rt::ViewId viewId, TrackId trackId);
    Gtk::Stack& _stack;
    rt::AppRuntime& _runtime;
    uimodel::playback::PlaybackQueueModel* _playbackQueueModel;
    TagEditController& _tagEditController;
    ListNavigationController& _listNavigation;
    uimodel::track::TrackPresentationViewModel& _presentationStore;
    rt::Subscription _revealSub;
    rt::Subscription _nowPlayingSub;
    rt::Subscription _focusSub;
    rt::Subscription _viewDestroyedSub;
    rt::Subscription _projectionChangedSub;
    rt::Subscription _presentationChangedSub;

    // Dedicated cache for section-header cover thumbnails (small, decode-at-scale
    // results). Destruction order is reverse declaration order:
    // _trackPages -> _thumbnailLoader -> _thumbnailCache. Keep these members in
    // this order so pages lose their widgets before the loader is cancelled, and
    // the loader is cancelled before the backing cache is destroyed.
    ImageCache _thumbnailCache{layout::kSectionThumbnailCacheCapacity};

    // Shared, off-thread thumbnail decoder over the cache above.
    ThumbnailLoader _thumbnailLoader{_runtime.musicLibrary(), _thumbnailCache, _runtime.async()};

    std::map<rt::ViewId, TrackPageContext> _trackPages;
    TrackId _playingTrackId{kInvalidTrackId};
    TrackRowCache* _activeDataProvider = nullptr;
  };
} // namespace ao::gtk
