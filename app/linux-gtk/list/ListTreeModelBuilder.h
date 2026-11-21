// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "list/ListNavigationSectionModel.h"
#include "list/ListTreeItem.h"
#include <ao/i18n/MessageCatalog.h>
#include <ao/uimodel/library/list/ListTreeProjection.h>

#include <giomm/liststore.h>
#include <glibmm/refptr.h>
#include <gtkmm/singleselection.h>
#include <gtkmm/treelistmodel.h>

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
      Glib::RefPtr<ListNavigationSectionModel> sectionModelPtr;
      Glib::RefPtr<Gtk::SingleSelection> selectionModelPtr;
      uimodel::ListTreeProjection projection;
    };

    static BuildResult build(rt::Library const& reads, i18n::MessageCatalog const& textCatalog);
  };
} // namespace ao::gtk
