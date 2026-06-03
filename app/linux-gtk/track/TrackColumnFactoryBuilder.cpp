// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackColumnFactoryBuilder.h"

#include "track/TrackFieldUi.h"
#include "track/TrackRowObject.h"
#include <ao/library/FileManifestLayout.h>
#include <ao/rt/TrackField.h>

#include <gdk/gdkkeysyms.h>
#include <gdkmm/enums.h>
#include <glibmm/refptr.h>
#include <gtkmm/entry.h>
#include <gtkmm/enums.h>
#include <gtkmm/eventcontrollerfocus.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/label.h>
#include <gtkmm/listitem.h>
#include <gtkmm/object.h>
#include <gtkmm/signallistitemfactory.h>
#include <gtkmm/stack.h>
#include <pangomm/layout.h>
#include <sigc++/scoped_connection.h>

#include <cstdint>
#include <memory>
#include <string>

namespace ao::gtk
{
  namespace
  {
    struct CellBindData final
    {
      sigc::scoped_connection activateConn;
      sigc::scoped_connection playingConn;

      void disconnectAll()
      {
        activateConn.disconnect();
        playingConn.disconnect();
      }
    };

    void updatePlayingStyles(Glib::RefPtr<Gtk::ListItem> const& listItem,
                             Glib::RefPtr<TrackRowObject> const& row,
                             rt::TrackField field)
    {
      if (auto* const child = listItem->get_child(); child != nullptr)
      {
        auto* const cell = child->get_parent();

        if (cell == nullptr)
        {
          return;
        }

        auto* const rowWidget = cell->get_parent();

        if (rowWidget == nullptr)
        {
          return;
        }

        if (row->isPlaying())
        {
          rowWidget->add_css_class("ao-playing-row");

          if (field == rt::TrackField::Title)
          {
            cell->add_css_class("ao-playing-title");
          }
          else
          {
            child->add_css_class("ao-playing-dim");
          }
        }
        else
        {
          rowWidget->remove_css_class("ao-playing-row");
          cell->remove_css_class("ao-playing-title");
          child->remove_css_class("ao-playing-dim");
        }
      }
    }

    void updateStatusStyles(Glib::RefPtr<Gtk::ListItem> const& listItem, Glib::RefPtr<TrackRowObject> const& row)
    {
      if (auto* const child = listItem->get_child(); child != nullptr)
      {
        auto* const cell = child->get_parent();

        if (cell == nullptr)
        {
          return;
        }

        auto* const rowWidget = cell->get_parent();

        if (rowWidget == nullptr)
        {
          return;
        }

        if (row->status() == library::FileStatus::Missing)
        {
          rowWidget->add_css_class("ao-missing-row");
        }
        else
        {
          rowWidget->remove_css_class("ao-missing-row");
        }
      }
    }

    void onTextColumnBindStatic(Glib::RefPtr<Gtk::ListItem> const& listItem,
                                rt::TrackField field,
                                Glib::RefPtr<TrackRowObject> const& row)
    {
      auto* const label = dynamic_cast<Gtk::Label*>(listItem->get_child());

      if (label != nullptr)
      {
        label->set_text(row->fieldText(field));
      }
    }

    void onTextColumnBind(Glib::RefPtr<Gtk::ListItem> const& listItem,
                          rt::TrackField field,
                          MetadataCommitFn const& commitFn)
    {
      auto const itemPtr = listItem->get_item();
      auto const rowPtr = std::dynamic_pointer_cast<TrackRowObject>(itemPtr);

      if (rowPtr == nullptr)
      {
        return;
      }

      auto* const bindData = static_cast<CellBindData*>(listItem->get_data(Glib::Quark{"bind-data"}));

      if (bindData != nullptr)
      {
        bindData->disconnectAll();
      }

      if (auto const* uiDef = trackFieldUiDefinition(field); uiDef == nullptr || !canInlineEdit(*uiDef))
      {
        onTextColumnBindStatic(listItem, field, rowPtr);
      }
      else
      {
        auto* const stack = dynamic_cast<Gtk::Stack*>(listItem->get_child());

        if (stack != nullptr)
        {
          stack->set_visible_child("display");

          auto* const label = dynamic_cast<Gtk::Label*>(stack->get_child_by_name("display"));
          auto* const entry = dynamic_cast<Gtk::Entry*>(stack->get_child_by_name("edit"));

          if (label != nullptr && entry != nullptr && bindData != nullptr)
          {
            auto const text = rowPtr->fieldText(field);
            label->set_text(text);
            entry->set_text(text);

            auto const commitChange = [entry, stack, label, rowPtr, field, commitFn]
            {
              auto const newValue = entry->get_text().raw();
              commitFn(rowPtr, field, newValue);
              auto const synced = rowPtr->fieldText(field);
              label->set_text(synced);
              entry->set_text(synced);
              stack->set_visible_child("display");
            };

            bindData->activateConn = entry->signal_activate().connect(commitChange);
          }
        }
      }

      updateStatusStyles(listItem, rowPtr);
      updatePlayingStyles(listItem, rowPtr, field);

      if (bindData != nullptr)
      {
        bindData->playingConn = rowPtr->property_playing().signal_changed().connect(
          [listItem, rowPtr, field] { updatePlayingStyles(listItem, rowPtr, field); });
      }
    }
  }

  Glib::RefPtr<Gtk::SignalListItemFactory> buildColumnFactory(rt::TrackField field, MetadataCommitFn const& commitFn)
  {
    auto factoryPtr = Gtk::SignalListItemFactory::create();

    factoryPtr->signal_setup().connect(
      [field](Glib::RefPtr<Gtk::ListItem> const& listItem)
      {
        if (auto const* uiDef = trackFieldUiDefinition(field); uiDef == nullptr || !canInlineEdit(*uiDef))
        {
          auto* const label = Gtk::make_managed<Gtk::Label>("");
          label->set_halign(Gtk::Align::START);
          label->set_ellipsize(Pango::EllipsizeMode::END);
          label->set_xalign(0);

          if (field == rt::TrackField::Duration || field == rt::TrackField::Year)
          {
            label->add_css_class("dim-label");
          }

          if (field == rt::TrackField::Tags)
          {
            label->add_css_class(kTagsCellCssClass);
          }

          listItem->set_child(*label);
        }
        else
        {
          auto* const stack = Gtk::make_managed<Gtk::Stack>();
          stack->set_vhomogeneous(false);
          stack->set_hhomogeneous(false);
          stack->set_transition_type(Gtk::StackTransitionType::NONE);
          stack->add_css_class("ao-inline-editor-stack");

          auto* const label = Gtk::make_managed<Gtk::Label>("");
          label->add_css_class("ao-inline-editor-label");
          label->set_halign(Gtk::Align::START);
          label->set_ellipsize(Pango::EllipsizeMode::END);
          stack->add(*label, "display");

          auto* const entry = Gtk::make_managed<Gtk::Entry>();
          entry->add_css_class("ao-inline-editor-entry");

          // Key controller created once per listItem, not per bind
          auto const keyControllerPtr = Gtk::EventControllerKey::create();
          keyControllerPtr->signal_key_pressed().connect(
            [stack](std::uint32_t keyval, std::uint32_t /*keycode*/, Gdk::ModifierType /*state*/)
            {
              if (keyval == GDK_KEY_Escape)
              {
                stack->set_visible_child("display");
                return true;
              }

              return false;
            },
            false);
          entry->add_controller(keyControllerPtr);

          auto const focusControllerPtr = Gtk::EventControllerFocus::create();
          focusControllerPtr->signal_leave().connect([stack] { stack->set_visible_child("display"); });
          entry->add_controller(focusControllerPtr);

          stack->add(*entry, "edit");

          listItem->set_child(*stack);
        }

        // Allocate connection storage once per listItem lifetime, reused across bind/unbind
        auto* const bindData = new CellBindData{};
        listItem->set_data(
          Glib::Quark{"bind-data"}, bindData, [](void* data) { delete static_cast<CellBindData*>(data); });
      });

    factoryPtr->signal_bind().connect([field, commitFn](Glib::RefPtr<Gtk::ListItem> const& listItem)
                                      { onTextColumnBind(listItem, field, commitFn); });

    factoryPtr->signal_unbind().connect(
      [](Glib::RefPtr<Gtk::ListItem> const& listItem)
      {
        if (auto* const bindData = static_cast<CellBindData*>(listItem->get_data(Glib::Quark{"bind-data"}));
            bindData != nullptr)
        {
          bindData->disconnectAll();
        }
      });

    return factoryPtr;
  }
} // namespace ao::gtk
