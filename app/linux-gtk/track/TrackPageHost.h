// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "image/ImageCache.h"
#include "image/ResourceImageLoader.h"
#include "layout/LayoutConstants.h"
#include "track/TrackViewPage.h"
#include <ao/CoreIds.h>
#include <ao/async/Subscription.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ViewIds.h>
#include <ao/uimodel/library/presentation/TrackColumnLayoutStore.h>

#include <map>
#include <memory>

namespace Gtk
{
  class Stack;
}

namespace ao::rt
{
  struct PlaybackRevealTrackRequest;
}

namespace ao::gtk
{
  class ListNavigationController;
  class TagEditController;
  class TrackRowCache;
  class TrackListModel;
  class TrackViewPage;

  /**
   * TrackPageEntry holds the per-page state for a track list.
   */
  struct TrackPageEntry final
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
                  TagEditController& tagEditController,
                  ListNavigationController& listNavigation,
                  uimodel::TrackColumnLayoutStore& layoutStore);
    ~TrackPageHost();

    // Not copyable or movable
    TrackPageHost(TrackPageHost const&) = delete;
    TrackPageHost& operator=(TrackPageHost const&) = delete;
    TrackPageHost(TrackPageHost&&) = delete;
    TrackPageHost& operator=(TrackPageHost&&) = delete;

    Gtk::Stack& stack() { return _stack; }
    uimodel::TrackColumnLayoutStore& layoutStore() { return _layoutStore; }

    void clear();
    void rebuild(TrackRowCache& dataProvider);

    TrackPageEntry* find(rt::ViewId viewId);
    TrackPageEntry const* find(rt::ViewId viewId) const;

    TrackPageEntry* currentVisible();
    TrackPageEntry const* currentVisible() const;

    void setPlayingTrack(TrackId trackId);

    /**
     * @return The list ID of the currently visible page, or rt::kAllTracksListId if none.
     */
    ListId activeListId() const;

  private:
    void ensureViewPage(rt::ViewId viewId, TrackRowCache& dataProvider);
    void bindTrackPage(TrackPageEntry& entry);
    void handleTrackSelectionChanged(TrackViewPage& page, rt::ViewId viewId);
    void syncLayout();
    void handleRevealTrack(rt::PlaybackRevealTrackRequest const& ev);
    rt::ViewId tryFindViewByPreferredList(ListId preferredListId);
    void tryRevealTrackInView(rt::ViewId viewId, TrackId trackId);
    Gtk::Stack& _stack;
    rt::AppRuntime& _runtime;
    TagEditController& _tagEditController;
    ListNavigationController& _listNavigation;
    uimodel::TrackColumnLayoutStore& _layoutStore;
    async::Subscription _revealSub;
    async::Subscription _snapshotSub;
    async::Subscription _focusSub;
    async::Subscription _viewDestroyedSub;
    async::Subscription _projectionChangedSub;
    async::Subscription _presentationChangedSub;

    // Dedicated cache for section-header cover thumbnails (small, decode-at-scale
    // results). Destruction order is reverse declaration order:
    // _trackPages -> _thumbnailLoader -> _thumbnailCache. Keep these members in
    // this order so pages lose their widgets before the loader is cancelled, and
    // the loader is cancelled before the backing cache is destroyed.
    ImageCache _thumbnailCache{layout::kSectionThumbnailCacheCapacity};

    // Shared, off-thread thumbnail decoder over the cache above.
    ResourceImageLoader _thumbnailLoader;

    std::map<rt::ViewId, TrackPageEntry> _trackPages;
    TrackId _playingTrackId{kInvalidTrackId};
    TrackRowCache* _activeDataProvider = nullptr;
  };
} // namespace ao::gtk
