// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "track/TrackRowCache.h"
#include <runtime/ProjectionTypes.h>

#include <giomm/listmodel.h>
#include <glibmm/object.h>

namespace ao::gtk
{
  class ProjectionTrackModel final
    : public Gio::ListModel
    , public Glib::Object
  {
  public:
    static Glib::RefPtr<ProjectionTrackModel> create();

    void setProjection(rt::ITrackListProjection* projection, TrackRowCache const& provider);
    void clearProjection();

    void notifyReset(::guint oldSize, ::guint newSize);
    void notifyInsert(::guint position, ::guint count);
    void notifyRemove(::guint position, ::guint count);
    void notifyUpdate(::guint position, ::guint count);

  protected:
    ProjectionTrackModel();

    ::GType get_item_type_vfunc() override;
    ::guint get_n_items_vfunc() override;
    ::gpointer get_item_vfunc(::guint position) override;

  private:
    rt::ITrackListProjection* _projection = nullptr;
    TrackRowCache const* _provider = nullptr;
    mutable ::GType _cachedItemType = G_TYPE_INVALID;
  };
} // namespace ao::gtk
