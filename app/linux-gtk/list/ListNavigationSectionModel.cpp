// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "list/ListNavigationSectionModel.h"

#include <giomm/listmodel.h>
#include <glib-object.h>
#include <glib.h>
#include <glibmm/objectbase.h>
#include <glibmm/refptr.h>
#include <gtkmm/sectionmodel.h>
#include <sigc++/functors/mem_fun.h>

namespace ao::gtk
{
  ListNavigationSectionModel::ListNavigationSectionModel()
    : Glib::ObjectBase{typeid(ListNavigationSectionModel)}, Gio::ListModel{}, Gtk::SectionModel{}
  {
  }

  Glib::RefPtr<ListNavigationSectionModel> ListNavigationSectionModel::create(
    Glib::RefPtr<Gio::ListModel> const& modelPtr)
  {
    auto sectionModelPtr = Glib::make_refptr_for_instance<ListNavigationSectionModel>(new ListNavigationSectionModel{});
    sectionModelPtr->_modelPtr = modelPtr;
    sectionModelPtr->_itemsChangedConnection = modelPtr->signal_items_changed().connect(
      sigc::mem_fun(*sectionModelPtr, &ListNavigationSectionModel::handleItemsChanged));
    return sectionModelPtr;
  }

  ::GType ListNavigationSectionModel::get_item_type_vfunc()
  {
    return _modelPtr->get_item_type();
  }

  ::guint ListNavigationSectionModel::get_n_items_vfunc()
  {
    return _modelPtr->get_n_items();
  }

  ::gpointer ListNavigationSectionModel::get_item_vfunc(::guint position)
  {
    auto const itemPtr = _modelPtr->get_object(position);

    if (!itemPtr)
    {
      return nullptr;
    }

    auto* const gobj = itemPtr->gobj();
    (::g_object_ref)(gobj); // NOLINT(readability-redundant-parentheses): bypass the GLib function-like macro.
    return gobj;
  }

  void ListNavigationSectionModel::get_section_vfunc(::guint position, ::guint& outStart, ::guint& outEnd)
  {
    auto const size = _modelPtr->get_n_items();

    if (position >= size)
    {
      outStart = size;
      outEnd = G_MAXUINT;
      return;
    }

    if (position == 0)
    {
      outStart = 0;
      outEnd = 1;
      return;
    }

    outStart = 1;
    outEnd = size;
  }

  void ListNavigationSectionModel::handleItemsChanged(::guint position, ::guint removed, ::guint added)
  {
    items_changed(position, removed, added);
  }
} // namespace ao::gtk
