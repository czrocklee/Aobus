// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "ListRow.h"

ListRow::ListRow()
  : _listId{ListId{0}}, _parentId{ListId{0}}, _depth{0}, _isSmart{false}, _name{}
{
}

Glib::RefPtr<ListRow> ListRow::create(ListId id, ListId parentId, int depth, bool smart, Glib::ustring const& name, Glib::ustring const& filter)
{
  auto obj = Glib::make_refptr_for_instance<ListRow>(new ListRow());
  obj->_listId = id;
  obj->_parentId = parentId;
  obj->_depth = depth;
  obj->_isSmart = smart;
  obj->_name = name;
  obj->_filter = filter;
  return obj;
}
