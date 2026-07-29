// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <giomm/listmodel.h>
#include <glibmm/object.h>
#include <glibmm/refptr.h>
#include <gtkmm/sectionmodel.h>
#include <sigc++/scoped_connection.h>

namespace ao::gtk
{
  class ListNavigationSectionModel final
    : public Gio::ListModel
    , public Gtk::SectionModel
    , public Glib::Object
  {
  public:
    static Glib::RefPtr<ListNavigationSectionModel> create(Glib::RefPtr<Gio::ListModel> const& modelPtr);

  protected:
    ListNavigationSectionModel();

    ::GType get_item_type_vfunc() override;
    ::guint get_n_items_vfunc() override;
    ::gpointer get_item_vfunc(::guint position) override;
    void get_section_vfunc(::guint position, ::guint& outStart, ::guint& outEnd) override;

  private:
    void handleItemsChanged(::guint position, ::guint removed, ::guint added);

    Glib::RefPtr<Gio::ListModel> _modelPtr;
    sigc::scoped_connection _itemsChangedConnection;
  };
} // namespace ao::gtk
