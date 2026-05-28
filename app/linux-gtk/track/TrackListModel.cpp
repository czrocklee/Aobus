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
#include <format>
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
    auto model = Glib::make_refptr_for_instance<TrackListModel>(new TrackListModel{});
    model->_provider = &provider;
    return model;
  }

  void TrackListModel::bindProjection(std::shared_ptr<rt::ITrackListProjection> projection)
  {
    _projectionSub.reset();

    auto const oldSize = static_cast<::guint>(_modelSize);
    auto const newSize = static_cast<::guint>(projection->size());

    _projection = std::move(projection);
    _modelSize = newSize;

    if (oldSize != newSize || oldSize > 0)
    {
      notifyReset(oldSize, newSize);
    }

    _projectionSub = _projection->subscribe(std::bind_front(&TrackListModel::applyDeltaBatch, this));
  }

  void TrackListModel::clearProjection()
  {
    _projectionSub.reset();
    _projection = nullptr;
    _modelSize = 0;
  }

  std::optional<std::size_t> TrackListModel::indexOf(TrackId trackId) const noexcept
  {
    return (_projection != nullptr) ? _projection->indexOf(trackId) : std::nullopt;
  }

  std::optional<std::size_t> TrackListModel::groupIndexForTrack(TrackId trackId) const noexcept
  {
    if (_projection == nullptr)
    {
      return std::nullopt;
    }

    if (auto const optIdx = _projection->indexOf(trackId))
    {
      return _projection->groupIndexAt(*optIdx);
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
    if (_projection != nullptr && _provider != nullptr && _projection->size() > 0)
    {
      auto const id = _projection->trackIdAt(0);

      if (auto const row = _provider->trackRow(id))
      {
        _cachedItemType = G_OBJECT_TYPE(row->gobj()); // NOLINT(cppcoreguidelines-pro-type-cstyle-cast)
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
    if (_projection == nullptr || _provider == nullptr)
    {
      return nullptr;
    }

    if (position >= _projection->size())
    {
      return nullptr;
    }

    auto const trackId = _projection->trackIdAt(position);
    auto const row = _provider->trackRow(trackId);

    if (!row)
    {
      return nullptr;
    }

    // Synchronize playing state before returning the object to the UI
    row->setPlaying(trackId == _playingTrackId);

    auto* const gobj = row->gobj();
    (g_object_ref)(gobj);
    return gobj;
  }

  void TrackListModel::setPlayingTrackId(TrackId id)
  {
    auto const oldId = _playingTrackId;
    _playingTrackId = id;

    if (_projection == nullptr)
    {
      return;
    }

    // Notify UI that the playing/previously-playing rows need a refresh
    if (oldId != kInvalidTrackId)
    {
      if (auto const optIdx = _projection->indexOf(oldId))
      {
        items_changed(static_cast<::guint>(*optIdx), 1, 1);
      }
    }

    if (id != kInvalidTrackId)
    {
      if (auto const optIdx = _projection->indexOf(id))
      {
        items_changed(static_cast<::guint>(*optIdx), 1, 1);
      }
    }
  }

  void TrackListModel::applyDeltaBatch(rt::TrackListProjectionDeltaBatch const& batch)
  {
    auto const timer = utility::ScopedTimer{
      std::format("TrackListModel::applyDeltas ({} deltas, rev={})", batch.deltas.size(), batch.revision)};

    for (auto const& delta : batch.deltas)
    {
      std::visit(utility::makeVisitor([this](rt::ProjectionReset const&) { applyResetDelta(); },
                                      std::bind_front(&TrackListModel::applyInsertRange, this),
                                      std::bind_front(&TrackListModel::applyRemoveRange, this),
                                      std::bind_front(&TrackListModel::applyUpdateRange, this)),
                 delta);
    }

    gsl_Expects(_modelSize == _projection->size());
  }

  void TrackListModel::applyResetDelta()
  {
    auto const newSize = static_cast<::guint>(_projection->size());
    auto const oldSize = static_cast<::guint>(_modelSize);
    notifyReset(oldSize, newSize);
    _modelSize = newSize;
  }

  void TrackListModel::applyInsertRange(rt::ProjectionInsertRange const& delta)
  {
    auto const pos = static_cast<::guint>(delta.range.start);
    auto const count = static_cast<::guint>(delta.range.count);
    notifyInsert(pos, count);
    _modelSize += count;
  }

  void TrackListModel::applyRemoveRange(rt::ProjectionRemoveRange const& delta)
  {
    auto const pos = static_cast<::guint>(delta.range.start);
    auto const count = static_cast<::guint>(delta.range.count);
    notifyRemove(pos, count);
    _modelSize -= count;
  }

  void TrackListModel::applyUpdateRange(rt::ProjectionUpdateRange const& delta)
  {
    auto const pos = static_cast<::guint>(delta.range.start);
    auto const count = static_cast<::guint>(delta.range.count);

    for (auto const idx : std::views::iota(delta.range.start, delta.range.start + delta.range.count))
    {
      _provider->invalidate(_projection->trackIdAt(idx));
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
