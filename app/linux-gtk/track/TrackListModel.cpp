// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "track/TrackListModel.h"

#include "track/TrackRowCache.h"
#include "track/TrackRowObject.h"
#include <ao/Type.h>
#include <ao/rt/ProjectionTypes.h>
#include <ao/utility/ScopedTimer.h>
#include <ao/utility/VariantVisitor.h>

#include <giomm/listmodel.h>
#include <giomm/private/listmodel_p.h>
#include <glib-object.h>
#include <glib.h>
#include <glibmm/objectbase.h>
#include <glibmm/refptr.h>
#include <gsl-lite/gsl-lite.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <utility>
#include <variant>

namespace
{
  Glib::Interface_Class const& getListModelIfaceClass()
  {
    static auto theClass = Gio::ListModel_Class{};
    return theClass.init();
  }
} // namespace

namespace ao::gtk
{
  TrackListModel::TrackListModel()
    : Glib::ObjectBase{typeid(TrackListModel)}, Gio::ListModel{getListModelIfaceClass()}
  {
  }

  Glib::RefPtr<TrackListModel> TrackListModel::create(TrackRowCache const& provider)
  {
    auto modelPtr = Glib::make_refptr_for_instance<TrackListModel>(new TrackListModel{});
    modelPtr->_provider = &provider;
    return modelPtr;
  }

  void TrackListModel::bindProjection(std::shared_ptr<rt::ITrackListProjection> projectionPtr)
  {
    _projectionSub.reset();

    auto const oldSize = static_cast<::guint>(_modelSize);
    auto const newSize = static_cast<::guint>(projectionPtr->size());

    _projectionPtr = std::move(projectionPtr);
    _modelSize = newSize;

    if (oldSize != newSize || oldSize > 0)
    {
      notifyReset(oldSize, newSize);
    }

    _projectionSub = _projectionPtr->subscribe(std::bind_front(&TrackListModel::applyDeltaBatch, this));
  }

  void TrackListModel::clearProjection()
  {
    _projectionSub.reset();
    _projectionPtr = nullptr;
    _modelSize = 0;
  }

  std::optional<std::size_t> TrackListModel::indexOf(TrackId trackId) const noexcept
  {
    return (_projectionPtr != nullptr) ? _projectionPtr->indexOf(trackId) : std::nullopt;
  }

  std::optional<std::size_t> TrackListModel::groupIndexForTrack(TrackId trackId) const noexcept
  {
    if (_projectionPtr == nullptr)
    {
      return std::nullopt;
    }

    if (auto const optIdx = _projectionPtr->indexOf(trackId); optIdx)
    {
      return _projectionPtr->groupIndexAt(*optIdx);
    }

    return std::nullopt;
  }

  ::GType TrackListModel::get_item_type_vfunc()
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
    if (_projectionPtr != nullptr && _provider != nullptr && _projectionPtr->size() > 0)
    {
      auto const id = _projectionPtr->trackIdAt(0);

      if (auto const rowPtr = _provider->trackRow(id); rowPtr)
      {
        _cachedItemType = G_OBJECT_TYPE(rowPtr->gobj()); // NOLINT(cppcoreguidelines-pro-type-cstyle-cast)
        return _cachedItemType;
      }
    }

    return G_TYPE_OBJECT;
  }

  ::guint TrackListModel::get_n_items_vfunc()
  {
    return static_cast<::guint>(_modelSize);
  }

  ::gpointer TrackListModel::get_item_vfunc(::guint position)
  {
    if (_projectionPtr == nullptr || _provider == nullptr)
    {
      return nullptr;
    }

    if (position >= _projectionPtr->size())
    {
      return nullptr;
    }

    auto const trackId = _projectionPtr->trackIdAt(position);
    auto const rowPtr = _provider->trackRow(trackId);

    if (!rowPtr)
    {
      return nullptr;
    }

    // Synchronize playing state before returning the object to the UI
    rowPtr->setPlaying(trackId == _playingTrackId);

    auto* const gobj = rowPtr->gobj();
    (::g_object_ref)(gobj);
    return gobj;
  }

  void TrackListModel::setPlayingTrackId(TrackId id)
  {
    auto const oldId = _playingTrackId;
    _playingTrackId = id;

    if (_projectionPtr == nullptr)
    {
      return;
    }

    // Notify UI that the playing/previously-playing rows need a refresh
    if (oldId != kInvalidTrackId)
    {
      if (auto const optIdx = _projectionPtr->indexOf(oldId); optIdx)
      {
        items_changed(static_cast<::guint>(*optIdx), 1, 1);
      }
    }

    if (id != kInvalidTrackId)
    {
      if (auto const optIdx = _projectionPtr->indexOf(id); optIdx)
      {
        items_changed(static_cast<::guint>(*optIdx), 1, 1);
      }
    }
  }

  void TrackListModel::applyDeltaBatch(rt::TrackListProjectionDeltaBatch const& batch)
  {
    auto const timer = utility::ScopedTimer{"TrackListModel::applyDeltas"};

    for (auto const& delta : batch.deltas)
    {
      std::visit(utility::makeVisitor([this](rt::ProjectionReset const&) { applyResetDelta(); },
                                      std::bind_front(&TrackListModel::applyInsertRange, this),
                                      std::bind_front(&TrackListModel::applyRemoveRange, this),
                                      std::bind_front(&TrackListModel::applyUpdateRange, this)),
                 delta);
    }

    gsl_Expects(_modelSize == _projectionPtr->size());
  }

  void TrackListModel::applyResetDelta()
  {
    auto const newSize = static_cast<::guint>(_projectionPtr->size());
    auto const oldSize = static_cast<::guint>(_modelSize);
    _modelSize = newSize;
    notifyReset(oldSize, newSize);
  }

  void TrackListModel::applyInsertRange(rt::ProjectionInsertRange const& delta)
  {
    auto const pos = static_cast<::guint>(delta.range.start);
    auto const count = static_cast<::guint>(delta.range.count);
    _modelSize += count;
    notifyInsert(pos, count);
  }

  void TrackListModel::applyRemoveRange(rt::ProjectionRemoveRange const& delta)
  {
    auto const pos = static_cast<::guint>(delta.range.start);
    auto const count = static_cast<::guint>(delta.range.count);
    _modelSize -= count;
    notifyRemove(pos, count);
  }

  void TrackListModel::applyUpdateRange(rt::ProjectionUpdateRange const& delta)
  {
    auto const pos = static_cast<::guint>(delta.range.start);
    auto const count = static_cast<::guint>(delta.range.count);

    for (auto const idx : std::views::iota(delta.range.start, delta.range.start + delta.range.count))
    {
      _provider->invalidate(_projectionPtr->trackIdAt(idx));
    }

    notifyUpdate(pos, count);
  }

  void TrackListModel::notifyReset(::guint oldSize, ::guint newSize)
  {
    items_changed(0, oldSize, newSize);
  }

  void TrackListModel::notifyInsert(::guint position, ::guint count)
  {
    items_changed(position, 0, count);
  }

  void TrackListModel::notifyRemove(::guint position, ::guint count)
  {
    items_changed(position, count, 0);
  }

  void TrackListModel::notifyUpdate(::guint position, ::guint count)
  {
    items_changed(position, count, count);
  }
} // namespace ao::gtk
