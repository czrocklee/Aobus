// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "list/ListRowObject.h"
#include <ao/CoreIds.h>

#include <giomm/liststore.h>
#include <glibmm/object.h>
#include <glibmm/refptr.h>

namespace ao::gtk
{
  class ListTreeItem final : public Glib::Object
  {
  public:
    Glib::RefPtr<ListRowObject> row() const { return _rowPtr; }

    ListId listId() const { return _rowPtr->listId(); }

    bool hasChildren() const { return _childrenPtr->get_n_items() > 0; }

    Glib::RefPtr<Gio::ListStore<ListTreeItem>> children() { return _childrenPtr; }

    static Glib::RefPtr<ListTreeItem> create(Glib::RefPtr<ListRowObject> const& rowPtr);

  protected:
    explicit ListTreeItem();

  private:
    Glib::RefPtr<ListRowObject> _rowPtr;
    Glib::RefPtr<Gio::ListStore<ListTreeItem>> _childrenPtr;
  };
} // namespace ao::gtk
