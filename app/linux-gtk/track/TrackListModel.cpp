// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "track/TrackListModel.h"
#include "track/TrackRowCache.h"
#include "track/TrackRowObject.h"
#include <ao/Type.h>
#include <runtime/ProjectionTypes.h>

#include <giomm/listmodel.h>
#include <giomm/private/listmodel_p.h>
#include <glib-object.h>
#include <glibmm/objectbase.h>
#include <glibmm/refptr.h>
#include <glib.h>



namespace
{
  Glib::Interface_Class const& getListModelIfaceClass()
  {
    static auto the_class = Gio::ListModel_Class{};
    return the_class.init();
  }
} // namespace

namespace ao::gtk
{
  ProjectionTrackModel::ProjectionTrackModel()
    : Glib::ObjectBase(typeid(ProjectionTrackModel)), Gio::ListModel(getListModelIfaceClass())
  {
  }

  Glib::RefPtr<ProjectionTrackModel> ProjectionTrackModel::create()
  {
    return Glib::make_refptr_for_instance<ProjectionTrackModel>(
      new ProjectionTrackModel()); // NOLINT(cppcoreguidelines-owning-memory)
  }

  void ProjectionTrackModel::setProjection(rt::ITrackListProjection* projection, TrackRowCache const& provider)
  {
    _projection = projection;
    _provider = &provider;
  }

  void ProjectionTrackModel::clearProjection()
  {
    _projection = nullptr;
    _provider = nullptr;
  }

  ::GType ProjectionTrackModel::get_item_type_vfunc()
  {
    if (_cachedItemType != G_TYPE_INVALID)
    {
      return _cachedItemType;
    }

    _cachedItemType = ::g_type_from_name("TrackRowObject");

    if (_cachedItemType != G_TYPE_INVALID)
    {
      return _cachedItemType;
    }

    // Try to resolve via a live row if available
    if (_projection != nullptr && _provider != nullptr && _projection->size() > 0)
    {
      auto const id = _projection->trackIdAt(0);

      if (auto const row = _provider->getTrackRow(id))
      {
        _cachedItemType = G_OBJECT_TYPE(row->gobj()); // NOLINT(cppcoreguidelines-pro-type-cstyle-cast)
        return _cachedItemType;
      }
    }

    return G_TYPE_OBJECT;
  }

  ::guint ProjectionTrackModel::get_n_items_vfunc()
  {
    return (_projection != nullptr) ? static_cast<::guint>(_projection->size()) : 0;
  }

  ::gpointer ProjectionTrackModel::get_item_vfunc(::guint position)
  {
    if (_projection == nullptr || _provider == nullptr)
    {
      return nullptr;
    }

    if (position >= _projection->size())
    {
      return nullptr;
    }

    auto const trackId = _projection->trackIdAt(position);
    auto const row = _provider->getTrackRow(trackId);

    if (!row)
    {
      return nullptr;
    }

    auto* const gobj = row->gobj();
    (g_object_ref)(gobj);
    return gobj;
  }

  void ProjectionTrackModel::notifyReset(::guint oldSize, ::guint newSize)
  {
    items_changed(0, oldSize, newSize);
  }

  void ProjectionTrackModel::notifyInsert(::guint position, ::guint count)
  {
    items_changed(position, 0, count);
  }

  void ProjectionTrackModel::notifyRemove(::guint position, ::guint count)
  {
    items_changed(position, count, 0);
  }

  void ProjectionTrackModel::notifyUpdate(::guint position, ::guint count)
  {
    items_changed(position, count, count);
  }
} // namespace ao::gtk
