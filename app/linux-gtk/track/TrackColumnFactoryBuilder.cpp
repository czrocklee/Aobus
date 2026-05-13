// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackColumnFactoryBuilder.h"

#include <gdk/gdk.h>
#include <gdkmm/contentprovider.h>
#include <glibmm/bytes.h>
#include <gtkmm/box.h>
#include <gtkmm/cssprovider.h>
#include <gtkmm/dragsource.h>
#include <gtkmm/entry.h>
#include <gtkmm/eventcontrollerfocus.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/gesturelongpress.h>
#include <gtkmm/label.h>
#include <gtkmm/listitem.h>
#include <gtkmm/stack.h>
#include <gtkmm/stylecontext.h>

#include <format>

namespace ao::gtk
{
  namespace
  {
    constexpr double kLongPressDelayFactor = 1.03;

    std::shared_ptr<Gdk::ContentProvider> createDragContentProvider(Gtk::Label* label, TrackColumn column)
    {
      auto const value = label->get_text().raw();

      if (value.empty())
      {
        return {};
      }

      auto prefix = std::string{};

      if (column == TrackColumn::Artist)
      {
        prefix = "$a=";
      }
      else if (column == TrackColumn::Album)
      {
        prefix = "$al=";
      }
      else if (column == TrackColumn::Genre)
      {
        prefix = "$g=";
      }

      if (prefix.empty())
      {
        return {};
      }

      auto const expr = std::format("{}\"{}\"", prefix, value);
      auto const bytes = Glib::Bytes::create(expr.data(), expr.size());

      return Gdk::ContentProvider::create("text/plain", bytes);
    }

    void ensureTrackPageCss(bool force = false)
    {
      static auto const provider = Gtk::CssProvider::create();
      static bool initialized = false;

      if (initialized && !force)
      {
        return;
      }

      if (force)
      {
        if (auto const display = Gdk::Display::get_default(); display)
        {
          Gtk::StyleContext::remove_provider_for_display(display, provider);
        }
      }

      provider->load_from_data(R"(
        /* 1. The Dynamic Beam: Seamlessly following the Title column via CSS variables */
        columnview row.playing-row {
          /* We use var(--ao-title-x) which is updated in real-time by C++ */
          background-image: linear-gradient(to right,
            transparent 0%,
            alpha(@warning_bg_color, 0.2) var(--ao-title-x, 35%),
            transparent 100%
          );
          background-color: transparent;
          border-color: transparent;
          transition: background-image 1.0s ease-out; /* Smooth sliding of the beam */
        }

        /* 2. Sharp Title Text */
        .playing-title {
          color: @theme_fg_color;
          font-weight: bold;
        }

        /* Sophisticated transition */
        columnview row {
          transition: all 450ms cubic-bezier(0.16, 1, 0.3, 1);
        }

        .inline-editor-stack { min-height: 0; margin: 0; }
        .inline-editor-label { border: 1px solid transparent; min-height: 0; }
        .inline-editor-entry {
          background: @view_bg_color;
          border: 1px solid @accent_color;
          border-radius: 4px;
          padding: 0 6px;
          margin: 0;
          min-height: 0;
          box-shadow: none;
          font-weight: bold;
        }

        .inline-editor-entry text { padding-top: 0; padding-bottom: 0; min-height: 0; }
      )");

      if (auto const display = Gdk::Display::get_default(); display)
      {
        Gtk::StyleContext::add_provider_for_display(display, provider, GTK_STYLE_PROVIDER_PRIORITY_USER);
      }

      initialized = true;
    }

    void onTextColumnSetup(Glib::RefPtr<Gtk::ListItem> const& listItem, TrackColumnDefinition const& definition)
    {
      if (definition.tagsCell)
      {
        auto* const box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 0);

        box->set_halign(Gtk::Align::FILL);
        box->set_hexpand(true);
        box->add_css_class("track-tags-cell");

        auto* const label = Gtk::make_managed<Gtk::Label>("");

        label->set_halign(Gtk::Align::START);
        label->set_ellipsize(Pango::EllipsizeMode::END);
        label->set_hexpand(true);

        box->append(*label);
        listItem->set_child(*box);

        return;
      }

      if (definition.editable)
      {
        auto* const stack = Gtk::make_managed<Gtk::Stack>();

        stack->add_css_class("inline-editor-stack");
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
        label->add_css_class("inline-editor-label");

        if (definition.draggable)
        {
          auto const source = Gtk::DragSource::create();

          source->signal_prepare().connect(
            sigc::slot<std::shared_ptr<Gdk::ContentProvider>(double, double)>(
              [label, column = definition.column](double, double) -> std::shared_ptr<Gdk::ContentProvider>
              { return createDragContentProvider(label, column); }),
            false);

          label->add_controller(source);
        }

        stack->add(*label, "display");

        auto* const entry = Gtk::make_managed<Gtk::Entry>();

        entry->add_css_class("inline-editor-entry");
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

      label->set_halign(definition.numeric ? Gtk::Align::END : Gtk::Align::START);
      label->set_xalign(definition.numeric ? 1.0F : 0.0F);

      if (!definition.numeric)
      {
        label->set_ellipsize(Pango::EllipsizeMode::END);
      }

      if (definition.draggable)
      {
        auto const source = Gtk::DragSource::create();

        source->signal_prepare().connect(
          sigc::slot<std::shared_ptr<Gdk::ContentProvider>(double, double)>(
            [label, column = definition.column](double, double) -> std::shared_ptr<Gdk::ContentProvider>
            { return createDragContentProvider(label, column); }),
          false);

        label->add_controller(source);
      }

      listItem->set_child(*label);
    }

    void onTextColumnBindEditable(Glib::RefPtr<Gtk::ListItem> const& listItem,
                                  TrackColumnDefinition const& definition,
                                  Glib::RefPtr<TrackRowObject> const& row,
                                  MetadataCommitFn const& commitFn)
    {
      auto* const stack = dynamic_cast<Gtk::Stack*>(listItem->get_child());
      auto* const label = (stack != nullptr) ? dynamic_cast<Gtk::Label*>(stack->get_child_by_name("display")) : nullptr;
      auto* const entry = (stack != nullptr) ? dynamic_cast<Gtk::Entry*>(stack->get_child_by_name("edit")) : nullptr;

      if (label != nullptr && entry != nullptr)
      {
        label->set_text(row->getColumnText(definition.column));
        entry->set_text(row->getColumnText(definition.column));

        auto const commitChange = [stack, entry, row, column = definition.column, commitFn]
        {
          if (stack->get_visible_child_name() != "edit")
          {
            return;
          }

          auto const newValue = entry->get_text().raw();
          stack->set_visible_child("display");

          commitFn(row, column, newValue);
        };

        auto const cancelChange = [stack, row, definition, entry]
        {
          stack->set_visible_child("display");
          entry->set_text(row->getColumnText(definition.column));
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

        if (definition.column == TrackColumn::Title)
        {
          modelConnection = row->property_title().signal_changed().connect(
            [label, entry, row]
            {
              label->set_text(row->getTitle());
              entry->set_text(row->getTitle());
            });
        }
        else if (definition.column == TrackColumn::Artist)
        {
          modelConnection = row->property_artist().signal_changed().connect(
            [label, entry, row]
            {
              label->set_text(row->getArtist());
              entry->set_text(row->getArtist());
            });
        }
        else if (definition.column == TrackColumn::Album)
        {
          modelConnection = row->property_album().signal_changed().connect(
            [label, entry, row]
            {
              label->set_text(row->getAlbum());
              entry->set_text(row->getAlbum());
            });
        }

        listItem->set_data("model-connection",
                           new sigc::connection{modelConnection}, // NOLINT(cppcoreguidelines-owning-memory)
                           [](::gpointer data)
                           {
                             auto* const conn =
                               static_cast<sigc::connection*>(data); // NOLINT(cppcoreguidelines-owning-memory)

                             conn->disconnect();
                             delete conn; // NOLINT(cppcoreguidelines-owning-memory)
                           });

        listItem->set_data("activate-connection",
                           new sigc::connection{activateConn}, // NOLINT(cppcoreguidelines-owning-memory)
                           [](::gpointer data)
                           {
                             auto* const conn =
                               static_cast<sigc::connection*>(data); // NOLINT(cppcoreguidelines-owning-memory)

                             conn->disconnect();
                             delete conn; // NOLINT(cppcoreguidelines-owning-memory)
                           });
      }
    }

    void onTextColumnBindStatic(Glib::RefPtr<Gtk::ListItem> const& listItem,
                                TrackColumnDefinition const& definition,
                                Glib::RefPtr<TrackRowObject> const& row)
    {
      auto* const label = dynamic_cast<Gtk::Label*>(listItem->get_child());

      if (label != nullptr)
      {
        label->set_text(row->getColumnText(definition.column));
      }
    }

    void onTextColumnBind(Glib::RefPtr<Gtk::ListItem> const& listItem,
                          TrackColumnDefinition const& definition,
                          MetadataCommitFn const& commitFn)
    {
      auto const item = listItem->get_item();
      auto const row = std::dynamic_pointer_cast<TrackRowObject>(item);

      if (row == nullptr)
      {
        return;
      }

      if (definition.tagsCell)
      {
        auto* const box = dynamic_cast<Gtk::Box*>(listItem->get_child());
        auto* const label = (box != nullptr) ? dynamic_cast<Gtk::Label*>(box->get_first_child()) : nullptr;

        if (label != nullptr)
        {
          label->set_text(row->getColumnText(definition.column));
        }
      }
      else if (definition.editable)
      {
        onTextColumnBindEditable(listItem, definition, row, commitFn);
      }
      else
      {
        onTextColumnBindStatic(listItem, definition, row);
      }

      auto const updateStyles = [listItem, row, definition]
      {
        auto* const child = listItem->get_child();

        if (row->isPlaying())
        {
          if (auto* const cell = child->get_parent())
          {
            if (auto* const rowWidget = cell->get_parent())
            {
              rowWidget->add_css_class("playing-row");
            }

            if (definition.column == TrackColumn::Title)
            {
              cell->add_css_class("playing-title");
            }
            else
            {
              child->add_css_class("playing-dim");
            }
          }
        }
        else
        {
          if (auto* const cell = child->get_parent())
          {
            if (auto* const rowWidget = cell->get_parent())
            {
              rowWidget->remove_css_class("playing-row");
            }

            cell->remove_css_class("playing-title");
          }

          child->remove_css_class("playing-dim");
        }
      };

      updateStyles();

      auto const connection = row->property_playing().signal_changed().connect(updateStyles);

      listItem->set_data("playing-connection",
                         new sigc::connection{connection}, // NOLINT(cppcoreguidelines-owning-memory)
                         [](::gpointer data)
                         {
                           auto* const conn =
                             static_cast<sigc::connection*>(data); // NOLINT(cppcoreguidelines-owning-memory)

                           conn->disconnect();
                           delete conn; // NOLINT(cppcoreguidelines-owning-memory)
                         });
    }
  } // namespace

  Glib::RefPtr<Gtk::SignalListItemFactory> buildColumnFactory(TrackColumnDefinition const& definition,
                                                              MetadataCommitFn const& commitFn)
  {
    ensureTrackPageCss();

    auto const factory = Gtk::SignalListItemFactory::create();

    factory->signal_setup().connect([definition](Glib::RefPtr<Gtk::ListItem> const& listItem)
                                    { onTextColumnSetup(listItem, definition); });

    factory->signal_bind().connect([definition, commitFn](Glib::RefPtr<Gtk::ListItem> const& listItem)
                                   { onTextColumnBind(listItem, definition, commitFn); });

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
