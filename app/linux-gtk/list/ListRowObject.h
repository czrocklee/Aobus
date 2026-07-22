// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>

#include <glibmm/object.h>
#include <glibmm/refptr.h>
#include <glibmm/ustring.h>

namespace ao::gtk
{
  class ListRowObject final : public Glib::Object
  {
  public:
    ListId listId() const { return _listId; }

    bool isSmart() const { return _isSmart; }

    Glib::ustring name() const { return _name; }

    Glib::ustring filter() const { return _filter; }

    static Glib::RefPtr<ListRowObject> create(ListId id,
                                              bool smart,
                                              Glib::ustring const& name,
                                              Glib::ustring const& filter = "");

  protected:
    explicit ListRowObject();

  private:
    ListId _listId;
    bool _isSmart = false;
    Glib::ustring _name;
    Glib::ustring _filter;
  };
} // namespace ao::gtk
