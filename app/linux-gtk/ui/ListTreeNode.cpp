// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "ListTreeNode.h"

#include "ListRow.h"

namespace ao::gtk
{
  ListTreeNode::ListTreeNode()
    : _children{Gio::ListStore<ListTreeNode>::create()}
  {
  }

  Glib::RefPtr<ListTreeNode> ListTreeNode::create(Glib::RefPtr<ListRow> const& row)
  {
    auto obj =
      Glib::make_refptr_for_instance<ListTreeNode>(new ListTreeNode()); // NOLINT(cppcoreguidelines-owning-memory)
    obj->_row = row;
    return obj;
  }

  Glib::RefPtr<ListTreeNode> ListTreeNode::getChild(guint index) const
  {
    if (index >= _children->get_n_items())
    {
      return nullptr;
    }

    return _children->get_item(index);
  }
} // namespace ao::gtk
