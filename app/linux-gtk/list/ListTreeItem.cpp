// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "list/ListTreeItem.h"

#include "list/ListRowObject.h"

namespace ao::gtk
{
  ListTreeItem::ListTreeItem()
    : _children{Gio::ListStore<ListTreeItem>::create()}
  {
  }

  Glib::RefPtr<ListTreeItem> ListTreeItem::create(Glib::RefPtr<ListRowObject> const& row)
  {
    auto obj =
      Glib::make_refptr_for_instance<ListTreeItem>(new ListTreeItem()); // NOLINT(cppcoreguidelines-owning-memory)
    obj->_row = row;
    return obj;
  }

  Glib::RefPtr<ListTreeItem> ListTreeItem::getChild(guint index) const
  {
    if (index >= _children->get_n_items())
    {
      return nullptr;
    }

    return _children->get_item(index);
  }
} // namespace ao::gtk
