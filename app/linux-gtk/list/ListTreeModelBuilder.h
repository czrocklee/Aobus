// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "list/ListTreeItem.h"
#include <ao/CoreIds.h>

#include <giomm/liststore.h>
#include <glibmm/refptr.h>
#include <gtkmm/singleselection.h>
#include <gtkmm/treelistmodel.h>

#include <map>

namespace ao::rt
{
  class Library;
}

namespace ao::gtk
{
  class ListTreeModelBuilder final
  {
  public:
    struct BuildResult final
    {
      Glib::RefPtr<Gio::ListStore<ListTreeItem>> storePtr;
      Glib::RefPtr<Gtk::TreeListModel> treeModelPtr;
      Glib::RefPtr<Gtk::SingleSelection> selectionModelPtr;
      std::map<ListId, Glib::RefPtr<ListTreeItem>> nodesById;
    };

    static BuildResult build(rt::Library const& reads);
  };
} // namespace ao::gtk
