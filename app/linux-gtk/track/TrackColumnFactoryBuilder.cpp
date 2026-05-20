// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackColumnFactoryBuilder.h"

#include "runtime/TrackField.h"
#include "track/TrackFieldUi.h"
#include "track/TrackRowObject.h"

#include <gdk/gdkkeysyms.h>
#include <gdkmm/contentprovider.h>
#include <gdkmm/enums.h>
#include <glib.h>
#include <glibmm/bytes.h>
#include <gtk/gtkstyleprovider.h>
#include <gtkmm/box.h>
#include <gtkmm/dragsource.h>
#include <gtkmm/entry.h>
#include <gtkmm/enums.h>
#include <gtkmm/eventcontrollerfocus.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/gesturelongpress.h>
#include <gtkmm/label.h>
#include <gtkmm/listitem.h>
#include <gtkmm/object.h>
#include <gtkmm/signallistitemfactory.h>
#include <gtkmm/stack.h>
#include <pangomm/layout.h>
#include <sigc++/connection.h>
#include <sigc++/functors/slot.h>

#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace ao::gtk
{
  namespace
  {
    constexpr double kLongPressDelayFactor = 1.03;

    std::shared_ptr<Gdk::ContentProvider> createDragContentProvider(Gtk::Label* label,
                                                                    std::string_view dragQueryPrefix)
    {
      auto const value = label->get_text().raw();

      if (value.empty())
      {
        return {};
      }

      if (dragQueryPrefix.empty())
      {
        return {};
      }

      auto const expr = std::format("{}\"{}\"", dragQueryPrefix, value);
      auto const bytes = Glib::Bytes::create(expr.data(), expr.size());

      return Gdk::ContentProvider::create("text/plain", bytes);
    }

    void destroyConnectionData(::gpointer data) noexcept
    {
      if (data == nullptr)
      {
        return;
      }

      auto conn = std::unique_ptr<sigc::connection>{static_cast<sigc::connection*>(data)};
      conn->disconnect();
    }

    void setConnectionData(Glib::RefPtr<Gtk::ListItem> const& listItem, char const* key, sigc::connection connection)
    {
      auto stored = std::make_unique<sigc::connection>(std::move(connection));
      listItem->set_data(key, stored.release(), destroyConnectionData);
    }

    void onTextColumnSetup(Glib::RefPtr<Gtk::ListItem> const& listItem, rt::TrackField field)
    {
      auto const* uiDef = trackFieldUiDefinition(field);

      if (uiDef != nullptr && uiDef->columnTagsCell)
      {
        auto* const box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 0);

        box->set_halign(Gtk::Align::FILL);
        box->set_hexpand(true);
        box->add_css_class("ao-track-tags-cell");

        auto* const label = Gtk::make_managed<Gtk::Label>("");

        label->set_halign(Gtk::Align::START);
        label->set_ellipsize(Pango::EllipsizeMode::END);
        label->set_hexpand(true);

        box->append(*label);
        listItem->set_child(*box);

        return;
      }

      auto const editable = uiDef != nullptr && uiDef->inlineEditable;

      if (editable)
      {
        auto* const stack = Gtk::make_managed<Gtk::Stack>();

        stack->add_css_class("ao-inline-editor-stack");
        stack->set_transition_type(Gtk::StackTransitionType::CROSSFADE);
        stack->set_vhomogeneous(false);
        stack->set_hexpand(true);
        stack->set_vexpand(true);
        stack->set_halign(Gtk::Align::FILL);
        stack->set_valign(Gtk::Align::FILL);

        auto* const label = Gtk::make_managed<Gtk::Label>("");

        label->set_halign(Gtk::Align::START);
        label->set_ellipsize(Pango::EllipsizeMode::END);
        label->set_hexpand(true);
        label->add_css_class("ao-inline-editor-label");

        auto const dragPrefix =
          uiDef != nullptr ? std::string{uiDef->dragQueryPrefix} : std::string{};

        if (!dragPrefix.empty())
        {
          auto const source = Gtk::DragSource::create();

          source->signal_prepare().connect(
            sigc::slot<std::shared_ptr<Gdk::ContentProvider>(double, double)>(
              [label, dragPrefix](double, double) -> std::shared_ptr<Gdk::ContentProvider>
              { return createDragContentProvider(label, dragPrefix); }),
            false);

          label->add_controller(source);
        }

        stack->add(*label, "display");

        auto* const entry = Gtk::make_managed<Gtk::Entry>();

        entry->add_css_class("ao-inline-editor-entry");
        entry->set_hexpand(true);
        entry->set_vexpand(true);
        entry->set_halign(Gtk::Align::FILL);
        entry->set_valign(Gtk::Align::FILL);

        stack->add(*entry, "edit");

        auto const longPress = Gtk::GestureLongPress::create();

        longPress->set_delay_factor(kLongPressDelayFactor);
        longPress->signal_pressed().connect(
          [stack, entry](double, double)
          {
            stack->set_visible_child("edit");
            entry->grab_focus();
            entry->select_region(0, -1);
          });

        stack->add_controller(longPress);

        listItem->set_child(*stack);

        return;
      }

      auto* const label = Gtk::make_managed<Gtk::Label>("");

      auto const numeric = uiDef != nullptr && uiDef->columnNumeric;

      label->set_halign(numeric ? Gtk::Align::END : Gtk::Align::START);
      label->set_xalign(numeric ? 1.0F : 0.0F);

      if (!numeric)
      {
        label->set_ellipsize(Pango::EllipsizeMode::END);
      }

      auto const dragPrefix =
        uiDef != nullptr ? std::string{uiDef->dragQueryPrefix} : std::string{};

      if (!dragPrefix.empty())
      {
        auto const source = Gtk::DragSource::create();

        source->signal_prepare().connect(
          sigc::slot<std::shared_ptr<Gdk::ContentProvider>(double, double)>(
            [label, dragPrefix](double, double) -> std::shared_ptr<Gdk::ContentProvider>
            { return createDragContentProvider(label, dragPrefix); }),
          false);

        label->add_controller(source);
      }

      listItem->set_child(*label);
    }

    void onTextColumnBindEditable(Glib::RefPtr<Gtk::ListItem> const& listItem,
                                  rt::TrackField field,
                                  Glib::RefPtr<TrackRowObject> const& row,
                                  MetadataCommitFn const& commitFn)
    {
      auto* const stack = dynamic_cast<Gtk::Stack*>(listItem->get_child());
      auto* const label = (stack != nullptr) ? dynamic_cast<Gtk::Label*>(stack->get_child_by_name("display")) : nullptr;
      auto* const entry = (stack != nullptr) ? dynamic_cast<Gtk::Entry*>(stack->get_child_by_name("edit")) : nullptr;

      if (label != nullptr && entry != nullptr)
      {
        label->set_text(row->getFieldText(field));
        entry->set_text(row->getFieldText(field));

        auto const commitChange = [stack, entry, row, field, commitFn]
        {
          if (stack->get_visible_child_name() != "edit")
          {
            return;
          }

          auto const newValue = entry->get_text().raw();
          stack->set_visible_child("display");

          commitFn(row, field, newValue);
        };

        auto const cancelChange = [stack, row, field, entry]
        {
          stack->set_visible_child("display");
          entry->set_text(row->getFieldText(field));
        };

        auto const activateConn = entry->signal_activate().connect(commitChange);
        auto const focusController = Gtk::EventControllerFocus::create();

        focusController->signal_leave().connect([commitChange] { commitChange(); });
        entry->add_controller(focusController);

        auto const keyController = Gtk::EventControllerKey::create();

        keyController->signal_key_pressed().connect(
          [cancelChange](guint keyval, guint, Gdk::ModifierType) -> bool
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

    void updatePlayingStyles(Glib::RefPtr<Gtk::ListItem> const& listItem,
                             Glib::RefPtr<TrackRowObject> const& row,
                             rt::TrackField field)
    {
      if (auto* const child = listItem->get_child(); row->isPlaying())
      {
        if (auto* const cell = child->get_parent())
        {
          if (auto* const rowWidget = cell->get_parent())
          {
            rowWidget->add_css_class("ao-playing-row");
          }

          if (field == rt::TrackField::Title)
          {
            cell->add_css_class("ao-playing-title");
          }
          else
          {
            child->add_css_class("ao-playing-dim");
          }
        }
      }
      else
      {
        if (auto* const cell = child->get_parent())
        {
          if (auto* const rowWidget = cell->get_parent())
          {
            rowWidget->remove_css_class("ao-playing-row");
          }

          cell->remove_css_class("ao-playing-title");
        }

        child->remove_css_class("ao-playing-dim");
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

      auto const* uiDef = trackFieldUiDefinition(field);
      auto const tagsCell = uiDef != nullptr && uiDef->columnTagsCell;
      auto const editable = uiDef != nullptr && uiDef->inlineEditable;

      if (tagsCell)
      {
        auto* const box = dynamic_cast<Gtk::Box*>(listItem->get_child());
        auto* const label = (box != nullptr) ? dynamic_cast<Gtk::Label*>(box->get_first_child()) : nullptr;

        if (label != nullptr)
        {
          label->set_text(row->getFieldText(field));
        }
      }
      else if (editable)
      {
        onTextColumnBindEditable(listItem, field, row, commitFn);
      }
      else
      {
        onTextColumnBindStatic(listItem, field, row);
      }

      updatePlayingStyles(listItem, row, field);

      auto const connection = row->property_playing().signal_changed().connect(
        [listItem, row, field] { updatePlayingStyles(listItem, row, field); });

      setConnectionData(listItem, "playing-connection", connection);
    }
  } // namespace

  Glib::RefPtr<Gtk::SignalListItemFactory> buildColumnFactory(rt::TrackField field,
                                                               MetadataCommitFn const& commitFn)
  {
    auto const factory = Gtk::SignalListItemFactory::create();

    factory->signal_setup().connect([field](Glib::RefPtr<Gtk::ListItem> const& listItem)
                                    { onTextColumnSetup(listItem, field); });

    factory->signal_bind().connect([field, commitFn](Glib::RefPtr<Gtk::ListItem> const& listItem)
                                   { onTextColumnBind(listItem, field, commitFn); });

    factory->signal_unbind().connect(
      [](Glib::RefPtr<Gtk::ListItem> const& listItem)
      {
        listItem->set_data("playing-connection", nullptr);
        listItem->set_data("activate-connection", nullptr);
        listItem->set_data("model-connection", nullptr);
      });

    return factory;
  }
} // namespace ao::gtk
