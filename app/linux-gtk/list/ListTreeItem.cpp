// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "list/ListTreeItem.h"

#include "list/ListRowObject.h"

#include <giomm/liststore.h>
#include <glib.h>
#include <glibmm/refptr.h>

namespace ao::gtk
{
  ListTreeItem::ListTreeItem()
    : Glib::ObjectBase{"ListTreeItem"}, _children{Gio::ListStore<ListTreeItem>::create()}
  {
  }

  Glib::RefPtr<ListTreeItem> ListTreeItem::create(Glib::RefPtr<ListRowObject> const& row)
  {
    auto obj = Glib::make_refptr_for_instance<ListTreeItem>(new ListTreeItem{});
    obj->_row = row;
    return obj;
  }

  Glib::RefPtr<ListTreeItem> ListTreeItem::child(guint index) const
  {
    if (index >= _children->get_n_items())
    {
      return nullptr;
    }

    return _children->get_item(index);
  }
} // namespace ao::gtk
