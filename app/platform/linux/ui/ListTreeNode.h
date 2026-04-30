// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "platform/linux/ui/ListRow.h"

#include <rs/library/MusicLibrary.h>

#include <gtkmm.h>

#include <memory>

namespace app::ui
{

  class ListTreeNode final : public Glib::Object
  {
  public:
    using ListId = rs::ListId;

    Glib::RefPtr<ListRow> getRow() const { return _row; }

    ListId getListId() const { return _row->getListId(); }

    bool hasChildren() const { return _children->get_n_items() > 0; }

    Glib::RefPtr<Gio::ListStore<ListTreeNode>> getChildren() { return _children; }

    guint getNChildren() const { return _children->get_n_items(); }

    Glib::RefPtr<ListTreeNode> getChild(guint index) const;

    void setParent(ListTreeNode* parent) { _parent = parent; }

    ListTreeNode* getParent() const { return _parent; }

    static Glib::RefPtr<ListTreeNode> create(Glib::RefPtr<ListRow> const& row);

  protected:
    explicit ListTreeNode();

  private:
    Glib::RefPtr<ListRow> _row;
    Glib::RefPtr<Gio::ListStore<ListTreeNode>> _children;
    ListTreeNode* _parent = nullptr;
  };

} // namespace app::ui
