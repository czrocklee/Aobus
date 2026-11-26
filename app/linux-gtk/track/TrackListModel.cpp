// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackListModel.h"

#include "track/TrackListItem.h"
#include "track/TrackRowCache.h"
#include "track/TrackRowPathDemand.h"
#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/rt/ScopedTimer.h>
#include <ao/rt/projection/TrackListProjection.h>
#include <ao/utility/VariantVisitor.h>

#include <giomm/listmodel.h>
#include <glib-object.h>
#include <glib.h>
#include <glibmm/main.h>
#include <glibmm/objectbase.h>
#include <glibmm/refptr.h>
#include <gtkmm/sectionmodel.h>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <variant>

namespace ao::gtk
{
  TrackListModel::TrackListModel()
    : Glib::ObjectBase{typeid(TrackListModel)}, Gio::ListModel{}, Gtk::SectionModel{}
  {
  }

  Glib::RefPtr<TrackListModel> TrackListModel::create(TrackRowCache const& provider)
  {
    auto modelPtr = Glib::make_refptr_for_instance<TrackListModel>(new TrackListModel{});
    modelPtr->_provider = &provider;
    return modelPtr;
  }

  void TrackListModel::bindProjection(std::unique_ptr<rt::TrackListProjection> projectionPtr)
  {
    // The model may have to defer the last release until after an invalidation
    // callback unwinds. Promote ownership at that boundary, not at callers that
    // otherwise own a transient projection exclusively.
    bindProjection(std::shared_ptr<rt::TrackListProjection>{std::move(projectionPtr)});
  }

  void TrackListModel::bindProjection(std::shared_ptr<rt::TrackListProjection const> projectionPtr)
  {
    AO_EXPECTS(projectionPtr != nullptr, "TrackListModel projection must not be null; use clearProjection()");
    _projectionSub.reset();
    _projectionPtr = std::move(projectionPtr);
    _projectionSub = _projectionPtr->subscribe(std::bind_front(&TrackListModel::applyDeltaBatch, this));
  }

  void TrackListModel::clearProjection()
  {
    auto const oldSize = static_cast<::guint>(_modelSize);
    _projectionSub.reset();
    _projectionPtr = nullptr;
    _modelSize = 0;

    if (oldSize != 0)
    {
      notifyReset(oldSize, 0);
    }
  }

  std::optional<std::size_t> TrackListModel::indexOf(TrackId trackId) const noexcept
  {
    return (_projectionPtr != nullptr) ? _projectionPtr->indexOf(trackId) : std::nullopt;
  }

  Glib::RefPtr<TrackRowObject> TrackListModel::trackRow(TrackId trackId, TrackRowPathDemand const pathDemand) const
  {
    return _provider->trackRow(trackId, pathDemand);
  }

  ::GType TrackListModel::get_item_type_vfunc()
  {
    return TrackListItem::objectType();
  }

  ::guint TrackListModel::get_n_items_vfunc()
  {
    return static_cast<::guint>(_modelSize);
  }

  ::gpointer TrackListModel::get_item_vfunc(::guint position)
  {
    if (_projectionPtr == nullptr || position >= _projectionPtr->size())
    {
      return nullptr;
    }

    auto const itemPtr = _items.get(_projectionPtr->trackIdAt(position));
    auto* const gobj = itemPtr->gobj();
    (::g_object_ref)(gobj); // NOLINT(readability-redundant-parentheses): bypass the GLib function-like macro.
    return gobj;
  }

  void TrackListModel::get_section_vfunc(::guint position, ::guint& outStart, ::guint& outEnd)
  {
    auto const size = static_cast<::guint>(_modelSize);

    if (_projectionPtr == nullptr || position >= size)
    {
      outStart = size;
      outEnd = G_MAXUINT;
      return;
    }

    if (auto const optGroupRange = _projectionPtr->groupRangeAt(position); optGroupRange)
    {
      outStart = std::min(static_cast<::guint>(optGroupRange->start), position);
      outEnd = std::clamp(static_cast<::guint>(optGroupRange->start + optGroupRange->count), position + 1, size);
      return;
    }

    AO_INVARIANT(_projectionPtr->groupCount() == 0);
    outStart = 0;
    outEnd = size;
  }

  void TrackListModel::setPlayingTrackId(TrackId id)
  {
    if (_playingTrackId == id)
    {
      return;
    }

    _playingTrackId = id;

    // A playing-state change does not replace a list item. Notify the cells directly.
    _playingChanged.emit();
  }

  void TrackListModel::applyDeltaBatch(rt::TrackListProjectionDeltaBatch const& batch)
  {
    auto const timer = rt::ScopedTimer{"TrackListModel::applyDeltas"};

    AO_INVARIANT(_projectionPtr != nullptr);
    AO_INVARIANT(rt::isValidTrackListProjectionDeltaBatch(batch, _modelSize));

    if (std::holds_alternative<rt::ProjectionSourceInvalidated>(batch.deltas.front()))
    {
      applySourceInvalidatedDelta();
      return;
    }

    _provider->prepareNotification(*_projectionPtr, batch);
    auto const containsOnlyUpdates =
      std::ranges::all_of(batch.deltas,
                          [](rt::TrackListProjectionDelta const& delta)
                          { return std::holds_alternative<rt::ProjectionUpdateRange>(delta); });

    if (batch.deltas.size() == 1 || containsOnlyUpdates)
    {
      for (auto const& delta : batch.deltas)
      {
        std::visit(
          utility::makeVisitor([this](rt::ProjectionReset const&) { applyResetDelta(); },
                               std::bind_front(&TrackListModel::applyInsertRange, this),
                               std::bind_front(&TrackListModel::applyRemoveRange, this),
                               std::bind_front(&TrackListModel::applyUpdateRange, this),
                               [](rt::ProjectionSourceInvalidated const&)
                               { AO_FATAL("TrackListModel cannot apply source invalidation as a regular delta"); }),
          delta);
      }
    }
    else
    {
      applyResetDelta();
    }

    AO_ENSURES(_modelSize == _projectionPtr->size());
  }

  void TrackListModel::applyResetDelta()
  {
    auto const newSize = static_cast<::guint>(_projectionPtr->size());
    auto const oldSize = static_cast<::guint>(_modelSize);
    _beforeReset.emit();
    _modelSize = newSize;
    notifyReset(oldSize, newSize);
  }

  void TrackListModel::applySourceInvalidatedDelta()
  {
    auto const oldSize = static_cast<::guint>(_modelSize);

    _projectionSub.reset();
    auto invalidatedProjectionPtr = std::move(_projectionPtr);

    // The callback may hold the model's last projection owner while the
    // projection is still emitting. Release that owner after this stack unwinds.
    Glib::signal_idle().connect_once(
      [invalidatedProjectionPtr = std::move(invalidatedProjectionPtr)] mutable
      { [[maybe_unused]] auto deferredProjectionPtr = std::move(invalidatedProjectionPtr); });

    _modelSize = 0;
    notifyReset(oldSize, 0);
  }

  void TrackListModel::applyInsertRange(rt::ProjectionInsertRange const& delta)
  {
    auto const rowPosition = static_cast<::guint>(delta.range.start);
    auto const count = static_cast<::guint>(delta.range.count);
    _modelSize += count;
    notifyInsert(rowPosition, count);
  }

  void TrackListModel::applyRemoveRange(rt::ProjectionRemoveRange const& delta)
  {
    auto const rowPosition = static_cast<::guint>(delta.range.start);
    auto const count = static_cast<::guint>(delta.range.count);
    _modelSize -= count;
    notifyRemove(rowPosition, count);
  }

  void TrackListModel::applyUpdateRange(rt::ProjectionUpdateRange const& delta)
  {
    auto const rowPosition = static_cast<::guint>(delta.range.start);
    auto const count = static_cast<::guint>(delta.range.count);

    notifyUpdate(rowPosition, count);
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
