// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "track/TrackListItem.h"

#include <ao/CoreIds.h>

#include <glib-object.h>
#include <glibmm/objectbase.h>
#include <glibmm/refptr.h>

#include <memory>
#include <unordered_map>
#include <utility>

namespace ao::gtk
{
  namespace
  {
    class ItemReference final
    {
    public:
      explicit ItemReference(TrackListItem& item) noexcept
        : _item{&item}
      {
        ::g_weak_ref_init(&_reference, item.gobj());
      }

      // The map constructs this wrapper in place so the GWeakRef stays at one address from init to clear.
      ItemReference(ItemReference const&) = delete;
      ItemReference& operator=(ItemReference const&) = delete;
      ItemReference(ItemReference&&) = delete;
      ItemReference& operator=(ItemReference&&) = delete;
      ~ItemReference() { ::g_weak_ref_clear(&_reference); }

      Glib::RefPtr<TrackListItem> lock()
      {
        // The new RefPtr adopts the native ref. The C++ wrapper is attached to
        // the GObject, not to any particular RefPtr control block.
        if (::g_weak_ref_get(&_reference) != nullptr)
        {
          return Glib::make_refptr_for_instance(_item);
        }

        return {};
      }

      bool matches(TrackListItem const* item) const noexcept { return _item == item; }

    private:
      TrackListItem* _item;
      ::GWeakRef _reference{};
    };
  } // namespace

  struct TrackListItem::Index final
  {
    std::unordered_map<TrackId, ItemReference> items;
  };

  TrackListItem::TrackListItem(TrackId id, std::weak_ptr<Index> indexWeakPtr)
    : Glib::ObjectBase{"TrackListItem"}, _id{id}, _indexWeakPtr{std::move(indexWeakPtr)}
  {
  }

  TrackListItem::~TrackListItem() noexcept
  {
    if (auto const indexPtr = _indexWeakPtr.lock(); indexPtr)
    {
      // Reentrant lookup during native disposal may already have replaced us.
      if (auto const position = indexPtr->items.find(_id);
          position != indexPtr->items.end() && position->second.matches(this))
      {
        indexPtr->items.erase(position);
      }
    }
  }

  ::GType TrackListItem::objectType()
  {
    static ::GType const kType = []
    {
      auto const itemPtr = Glib::make_refptr_for_instance(new TrackListItem{kInvalidTrackId});
      // G_OBJECT_TYPE is a GLib function-like macro with an unavoidable C cast.
      return G_OBJECT_TYPE(itemPtr->gobj());
    }();
    return kType;
  }

  TrackListItemPool::TrackListItemPool()
    : _indexPtr{std::make_shared<TrackListItem::Index>()}
  {
  }

  Glib::RefPtr<TrackListItem> TrackListItemPool::get(TrackId id) const
  {
    auto& items = _indexPtr->items;

    if (auto const position = items.find(id); position != items.end())
    {
      if (auto itemPtr = position->second.lock(); itemPtr)
      {
        return itemPtr;
      }

      items.erase(position);
    }

    auto itemPtr = Glib::make_refptr_for_instance(new TrackListItem{id, _indexPtr});
    items.try_emplace(id, *itemPtr);
    return itemPtr;
  }
} // namespace ao::gtk
