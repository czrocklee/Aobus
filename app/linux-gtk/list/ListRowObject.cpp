// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "list/ListRowObject.h"

namespace ao::gtk
{
  ListRowObject::ListRowObject()
    : _listId{ListId{0}}, _parentId{ListId{0}}
  {
  }

  Glib::RefPtr<ListRowObject> ListRowObject::create(ListId id,
                                        ListId parentId,
                                        std::int32_t depth,
                                        bool smart,
                                        Glib::ustring const& name,
                                        Glib::ustring const& filter)
  {
    auto obj = Glib::make_refptr_for_instance<ListRowObject>(new ListRowObject{}); // NOLINT(cppcoreguidelines-owning-memory)
    obj->_listId = id;
    obj->_parentId = parentId;
    obj->_depth = depth;
    obj->_isSmart = smart;
    obj->_name = name;
    obj->_filter = filter;
    return obj;
  }
} // namespace ao::gtk
