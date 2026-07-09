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
    : Glib::ObjectBase{"ListTreeItem"}, _childrenPtr{Gio::ListStore<ListTreeItem>::create()}
  {
  }

  Glib::RefPtr<ListTreeItem> ListTreeItem::create(Glib::RefPtr<ListRowObject> const& rowPtr)
  {
    auto objPtr = Glib::make_refptr_for_instance<ListTreeItem>(new ListTreeItem{});
    objPtr->_rowPtr = rowPtr;
    return objPtr;
  }

  Glib::RefPtr<ListTreeItem> ListTreeItem::child(guint index) const
  {
    if (index >= _childrenPtr->get_n_items())
    {
      return nullptr;
    }

    return _childrenPtr->get_item(index);
  }
} // namespace ao::gtk
