// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "list/ListRowObject.h"

#include <ao/library/MusicLibrary.h>

#include <gtkmm.h>

namespace ao::gtk
{
  class ListTreeItem final : public Glib::Object
  {
  public:
    Glib::RefPtr<ListRowObject> getRow() const { return _row; }

    ListId getListId() const { return _row->getListId(); }

    bool hasChildren() const { return _children->get_n_items() > 0; }

    Glib::RefPtr<Gio::ListStore<ListTreeItem>> getChildren() { return _children; }

    guint getNChildren() const { return _children->get_n_items(); }

    Glib::RefPtr<ListTreeItem> getChild(guint index) const;

    void setParent(ListTreeItem* parent) { _parent = parent; }

    ListTreeItem* getParent() const { return _parent; }

    static Glib::RefPtr<ListTreeItem> create(Glib::RefPtr<ListRowObject> const& row);

  protected:
    explicit ListTreeItem();

  private:
    Glib::RefPtr<ListRowObject> _row;
    Glib::RefPtr<Gio::ListStore<ListTreeItem>> _children;
    ListTreeItem* _parent = nullptr;
  };
} // namespace ao::gtk
