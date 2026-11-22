// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>

#include <glibmm/object.h>
#include <glibmm/refptr.h>

#include <memory>

namespace ao::gtk
{
  class TrackListItem final : public Glib::Object
  {
  public:
    TrackListItem(TrackListItem const&) = delete;
    TrackListItem& operator=(TrackListItem const&) = delete;
    TrackListItem(TrackListItem&&) = delete;
    TrackListItem& operator=(TrackListItem&&) = delete;
    ~TrackListItem() noexcept override;

    TrackId trackId() const noexcept { return _id; }
    static ::GType objectType();

  private:
    friend class TrackListItemPool;
    struct Index;

    explicit TrackListItem(TrackId id, std::weak_ptr<Index> indexWeakPtr = {});

    TrackId _id;
    // A cleanup link only; the model may retire before GTK releases its items.
    std::weak_ptr<Index> _indexWeakPtr;
  };

  // Per-model identity index. Lookup and last native unref stay on the GTK thread.
  class TrackListItemPool final
  {
  public:
    TrackListItemPool();

    TrackListItemPool(TrackListItemPool const&) = delete;
    TrackListItemPool& operator=(TrackListItemPool const&) = delete;
    TrackListItemPool(TrackListItemPool&&) = delete;
    TrackListItemPool& operator=(TrackListItemPool&&) = delete;
    ~TrackListItemPool() = default;

    Glib::RefPtr<TrackListItem> get(TrackId id) const;

  private:
    std::shared_ptr<TrackListItem::Index> _indexPtr;
  };
} // namespace ao::gtk
