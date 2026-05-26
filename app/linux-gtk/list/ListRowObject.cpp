// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "list/ListRowObject.h"

#include "ao/Type.h"

#include <glibmm/refptr.h>
#include <glibmm/ustring.h>

#include <cstdint>

namespace ao::gtk
{
  ListRowObject::ListRowObject()
    : Glib::ObjectBase{"ListRowObject"}, _listId{kInvalidListId}, _parentId{kInvalidListId}
  {
  }

  Glib::RefPtr<ListRowObject> ListRowObject::create(ListId id,
                                                    ListId parentId,
                                                    std::int32_t depth,
                                                    bool smart,
                                                    Glib::ustring const& name,
                                                    Glib::ustring const& filter)
  {
    auto obj = Glib::make_refptr_for_instance<ListRowObject>(new ListRowObject{});
    obj->_listId = id;
    obj->_parentId = parentId;
    obj->_depth = depth;
    obj->_isSmart = smart;
    obj->_name = name;
    obj->_filter = filter;
    return obj;
  }
} // namespace ao::gtk
