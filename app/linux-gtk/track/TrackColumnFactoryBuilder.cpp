// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackColumnFactoryBuilder.h"

#include "ao/library/FileManifestLayout.h"
#include "runtime/TrackField.h"
#include "track/TrackFieldUi.h"
#include "track/TrackRowObject.h"

#include <gdk/gdkkeysyms.h>
#include <gdkmm/enums.h>
#include <glibmm/refptr.h>
#include <gtkmm/entry.h>
#include <gtkmm/enums.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/label.h>
#include <gtkmm/listitem.h>
#include <gtkmm/object.h>
#include <gtkmm/signallistitemfactory.h>
#include <gtkmm/stack.h>
#include <pangomm/layout.h>
#include <sigc++/connection.h>

#include <cstdint>
#include <memory>
#include <string>

namespace ao::gtk
{
  namespace
  {
    void setConnectionData(Glib::RefPtr<Gtk::ListItem> const& listItem,
                           std::string const& key,
                           sigc::connection const& conn)
    {
      listItem->set_data(
        Glib::Quark{key}, new sigc::connection{conn}, [](void* data) { delete static_cast<sigc::connection*>(data); });
    }

    void updatePlayingStyles(Glib::RefPtr<Gtk::ListItem> const& listItem,
                             Glib::RefPtr<TrackRowObject> const& row,
                             rt::TrackField field)
    {
      if (auto* const child = listItem->get_child())
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
      if (auto* const child = listItem->get_child())
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

        if (row->getStatus() == library::FileStatus::Missing)
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
        label->set_text(row->getFieldText(field));
      }
    }

    void onTextColumnBind(Glib::RefPtr<Gtk::ListItem> const& listItem,
                          rt::TrackField field,
                          MetadataCommitFn const& commitFn)
    {
      auto const item = listItem->get_item();
      auto const row = std::dynamic_pointer_cast<TrackRowObject>(item);

      if (row == nullptr)
      {
        return;
      }

      if (auto const* uiDef = trackFieldUiDefinition(field); uiDef == nullptr || !uiDef->inlineEditable)
      {
        onTextColumnBindStatic(listItem, field, row);
      }
      else
      {
        auto* const stack = dynamic_cast<Gtk::Stack*>(listItem->get_child());

        if (stack != nullptr)
        {
          auto* const label = dynamic_cast<Gtk::Label*>(stack->get_child_by_name("display"));
          auto* const entry = dynamic_cast<Gtk::Entry*>(stack->get_child_by_name("edit"));

          if (label != nullptr)
          {
            label->set_text(row->getFieldText(field));
          }

          if (entry != nullptr)
          {
            entry->set_text(row->getFieldText(field));
          }

          auto const commitChange = [entry, stack, row, field, commitFn]
          {
            auto const newValue = entry->get_text().raw();
            commitFn(row, field, newValue);
            stack->set_visible_child("display");
          };

          auto const cancelChange = [stack] { stack->set_visible_child("display"); };

          auto const activateConn = entry->signal_activate().connect(commitChange);

          auto const keyController = Gtk::EventControllerKey::create();
          keyController->signal_key_pressed().connect(
            [cancelChange](std::uint32_t keyval, std::uint32_t /*keycode*/, Gdk::ModifierType /*state*/)
            {
              if (keyval == GDK_KEY_Escape)
              {
                cancelChange();
                return true;
              }

              return false;
            },
            false);
          entry->add_controller(keyController);

          auto modelConnection = sigc::connection{};

          if (field == rt::TrackField::Title)
          {
            modelConnection = row->property_title().signal_changed().connect(
              [label, entry, row]
              {
                label->set_text(row->getTitle());
                entry->set_text(row->getTitle());
              });
          }
          else if (field == rt::TrackField::Artist)
          {
            modelConnection = row->property_artist().signal_changed().connect(
              [label, entry, row]
              {
                label->set_text(row->getArtist());
                entry->set_text(row->getArtist());
              });
          }
          else if (field == rt::TrackField::Album)
          {
            modelConnection = row->property_album().signal_changed().connect(
              [label, entry, row]
              {
                label->set_text(row->getAlbum());
                entry->set_text(row->getAlbum());
              });
          }

          setConnectionData(listItem, "model-connection", modelConnection);
          setConnectionData(listItem, "activate-connection", activateConn);
        }
      }

      updateStatusStyles(listItem, row);
      updatePlayingStyles(listItem, row, field);

      auto const playingConn = row->property_playing().signal_changed().connect(
        [listItem, row, field] { updatePlayingStyles(listItem, row, field); });

      setConnectionData(listItem, "playing-connection", playingConn);
    }
  }

  Glib::RefPtr<Gtk::SignalListItemFactory> buildColumnFactory(rt::TrackField field, MetadataCommitFn const& commitFn)
  {
    auto factory = Gtk::SignalListItemFactory::create();

    factory->signal_setup().connect(
      [field](Glib::RefPtr<Gtk::ListItem> const& listItem)
      {
        if (auto const* uiDef = trackFieldUiDefinition(field); uiDef == nullptr || !uiDef->inlineEditable)
        {
          auto* const label = Gtk::make_managed<Gtk::Label>("");
          label->set_halign(Gtk::Align::START);
          label->set_ellipsize(Pango::EllipsizeMode::END);
          label->set_xalign(0);

          if (field == rt::TrackField::Duration || field == rt::TrackField::Year)
          {
            label->add_css_class("dim-label");
          }

          listItem->set_child(*label);
          return;
        }

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
        stack->add(*entry, "edit");

        listItem->set_child(*stack);
      });

    factory->signal_bind().connect([field, commitFn](Glib::RefPtr<Gtk::ListItem> const& listItem)
                                   { onTextColumnBind(listItem, field, commitFn); });

    return factory;
  }
} // namespace ao::gtk
