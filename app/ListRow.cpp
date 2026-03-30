// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "ListRow.h"

ListRow::ListRow()
  : _listId{ListId{0}}, _name{}
{
}

Glib::RefPtr<ListRow> ListRow::create(ListId id, Glib::ustring const& name)
{
  auto obj = Glib::make_refptr_for_instance<ListRow>(new ListRow());
  obj->_listId = id;
  obj->_name = name;
  return obj;
}
