// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "platform/linux/ui/ListTreeNode.h"

#include "platform/linux/ui/ListRow.h"

namespace app::ui
{

  ListTreeNode::ListTreeNode()
    : _row{}, _children{Gio::ListStore<ListTreeNode>::create()}, _parent{nullptr}
  {
  }

  Glib::RefPtr<ListTreeNode> ListTreeNode::create(Glib::RefPtr<ListRow> const& row)
  {
    auto obj = Glib::make_refptr_for_instance<ListTreeNode>(new ListTreeNode());
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

} // namespace app::ui
