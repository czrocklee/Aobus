// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/rt/Subscription.h>
#include <ao/rt/projection/TrackListProjection.h>

#include <giomm/listmodel.h>
#include <glib-object.h>
#include <glib.h>
#include <glibmm/object.h>
#include <glibmm/refptr.h>
#include <sigc++/signal.h>

#include <cstddef>
#include <memory>
#include <optional>

namespace ao::gtk
{
  class TrackRowCache;

  class TrackListModel final
    : public Gio::ListModel
    , public Glib::Object
  {
  public:
    static Glib::RefPtr<TrackListModel> create(TrackRowCache const& provider);

    void bindProjection(std::shared_ptr<rt::TrackListProjection> projectionPtr);
    void clearProjection();

    rt::TrackListProjection* projection() const noexcept { return _projectionPtr.get(); }

    std::optional<std::size_t> indexOf(TrackId trackId) const noexcept;
    std::optional<std::size_t> groupIndexForTrack(TrackId trackId) const noexcept;

    /// Update the track rendered as now-playing. This emits signalPlayingChanged()
    /// instead of items_changed(); visible cells restyle from playingTrackId(), and
    /// newly-bound/recycled cells are stamped in get_item_vfunc().
    void setPlayingTrackId(TrackId id);

    /// The track currently rendered as "playing", or kInvalidTrackId.
    TrackId playingTrackId() const noexcept { return _playingTrackId; }

    /// Emitted when the playing track changes. Cells subscribe once (at setup) to
    /// restyle in place, since the shared, cached row objects make GTK skip a
    /// rebind on items_changed.
    sigc::signal<void()>& signalPlayingChanged() noexcept { return _playingChanged; }

    void notifyReset(::guint oldSize, ::guint newSize);
    void notifyInsert(::guint position, ::guint count);
    void notifyRemove(::guint position, ::guint count);
    void notifyUpdate(::guint position, ::guint count);

  protected:
    TrackListModel();

    ::GType get_item_type_vfunc() override;
    ::guint get_n_items_vfunc() override;
    ::gpointer get_item_vfunc(::guint position) override;

  private:
    void applyDeltaBatch(rt::TrackListProjectionDeltaBatch const& batch);
    void applyResetDelta();
    void applyInsertRange(rt::ProjectionInsertRange const& delta);
    void applyRemoveRange(rt::ProjectionRemoveRange const& delta);
    void applyUpdateRange(rt::ProjectionUpdateRange const& delta);

    std::shared_ptr<rt::TrackListProjection> _projectionPtr;
    rt::Subscription _projectionSub;
    TrackRowCache const* _provider = nullptr;
    mutable ::GType _cachedItemType = G_TYPE_INVALID;
    TrackId _playingTrackId{kInvalidTrackId};
    sigc::signal<void()> _playingChanged;
    std::size_t _modelSize = 0;
  };
} // namespace ao::gtk
