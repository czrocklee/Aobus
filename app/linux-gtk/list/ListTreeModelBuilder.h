// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "list/ListTreeItem.h"
#include <ao/Type.h>
#include <map>

namespace ao::lmdb
{
  class ReadTransaction;
}

namespace ao::rt
{
  class AppSession;
}

namespace ao::gtk
{
  class ListTreeModelBuilder final
  {
  public:
    struct Result final
    {
      Glib::RefPtr<Gio::ListStore<ListTreeItem>> store;
      Glib::RefPtr<Gtk::TreeListModel> treeModel;
      Glib::RefPtr<Gtk::SingleSelection> selectionModel;
      std::map<ListId, Glib::RefPtr<ListTreeItem>> nodesById;
    };

    static Result build(ao::rt::AppSession& session, ao::lmdb::ReadTransaction const& txn);
  };
} // namespace ao::gtk
