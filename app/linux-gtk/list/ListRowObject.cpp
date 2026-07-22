// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "list/ListRowObject.h"

#include <ao/CoreIds.h>

#include <glibmm/refptr.h>
#include <glibmm/ustring.h>

namespace ao::gtk
{
  ListRowObject::ListRowObject()
    : Glib::ObjectBase{"ListRowObject"}, _listId{kInvalidListId}
  {
  }

  Glib::RefPtr<ListRowObject> ListRowObject::create(ListId id,
                                                    bool smart,
                                                    Glib::ustring const& name,
                                                    Glib::ustring const& filter)
  {
    auto objPtr = Glib::make_refptr_for_instance<ListRowObject>(new ListRowObject{});
    objPtr->_listId = id;
    objPtr->_isSmart = smart;
    objPtr->_name = name;
    objPtr->_filter = filter;
    return objPtr;
  }
} // namespace ao::gtk
