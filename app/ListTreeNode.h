// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/core/MusicLibrary.h>

#include <gtkmm.h>

#include <memory>

#include "ListRow.h"

class ListTreeNode final : public Glib::Object
{
public:
  using ListId = rs::core::ListId;

  static Glib::RefPtr<ListTreeNode> create(Glib::RefPtr<ListRow> const& row);

  Glib::RefPtr<ListRow> getRow() const { return _row; }

  ListId getListId() const { return _row->getListId(); }

  bool hasChildren() const { return _children->get_n_items() > 0; }

  Glib::RefPtr<Gio::ListStore<ListTreeNode>> getChildren() { return _children; }

  guint getNChildren() const { return _children->get_n_items(); }

  Glib::RefPtr<ListTreeNode> getChild(guint index) const;

  void setParent(ListTreeNode* parent) { _parent = parent; }

  ListTreeNode* getParent() const { return _parent; }

protected:
  explicit ListTreeNode();

private:
  Glib::RefPtr<ListRow> _row;
  Glib::RefPtr<Gio::ListStore<ListTreeNode>> _children;
  ListTreeNode* _parent = nullptr;
};
