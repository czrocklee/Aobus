// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "ao/Type.h"
#include "track/TrackRowCache.h"
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/ProjectionTypes.h>

#include <giomm/listmodel.h>
#include <glib-object.h>
#include <glib.h>
#include <glibmm/object.h>
#include <glibmm/refptr.h>

#include <cstddef>
#include <memory>
#include <optional>

namespace ao::gtk
{
  class TrackListModel final
    : public Gio::ListModel
    , public Glib::Object
  {
  public:
    static Glib::RefPtr<TrackListModel> create(TrackRowCache const& provider);

    void bindProjection(std::shared_ptr<rt::ITrackListProjection> projection);
    void clearProjection();

    rt::ITrackListProjection* projection() const noexcept { return _projection.get(); }

    std::optional<std::size_t> indexOf(TrackId trackId) const noexcept;
    std::optional<std::size_t> groupIndexForTrack(TrackId trackId) const noexcept;

    void setPlayingTrackId(TrackId id);

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

    std::shared_ptr<rt::ITrackListProjection> _projection;
    rt::Subscription _projectionSub;
    TrackRowCache const* _provider = nullptr;
    mutable ::GType _cachedItemType = G_TYPE_INVALID;
    TrackId _playingTrackId{kInvalidTrackId};
    std::size_t _modelSize = 0;
  };
} // namespace ao::gtk
