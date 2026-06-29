// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "list/ListRowObject.h"
#include <ao/CoreIds.h>

#include <giomm/liststore.h>
#include <glib.h>
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

    guint nChildren() const { return _childrenPtr->get_n_items(); }

    Glib::RefPtr<ListTreeItem> child(guint index) const;

    void setParent(ListTreeItem* parent) { _parent = parent; }

    ListTreeItem* parent() const { return _parent; }

    static Glib::RefPtr<ListTreeItem> create(Glib::RefPtr<ListRowObject> const& row);

  protected:
    explicit ListTreeItem();

  private:
    Glib::RefPtr<ListRowObject> _rowPtr;
    Glib::RefPtr<Gio::ListStore<ListTreeItem>> _childrenPtr;
    ListTreeItem* _parent = nullptr;
  };
} // namespace ao::gtk
