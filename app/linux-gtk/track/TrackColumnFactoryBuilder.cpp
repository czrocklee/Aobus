// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackColumnFactoryBuilder.h"

#include "track/TrackFieldUi.h"
#include "track/TrackListModel.h"
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
      // Both connections are established once at setup and held for the cell's
      // lifetime; the scoped_connections auto-disconnect when this struct (owned
      // by the ListItem) is destroyed. Neither is touched by bind/unbind: the
      // slots resolve the currently-bound row lazily from listItem->get_item(),
      // so a recycled cell needs no reconnect.
      sigc::scoped_connection commitConnection;         // inline-edit commit (Enter)
      sigc::scoped_connection playingChangedConnection; // now-playing highlight
    };

    void configureTrackCellLabel(Gtk::Label& label)
    {
      label.set_halign(Gtk::Align::START);
      label.set_ellipsize(Pango::EllipsizeMode::END);
      label.set_single_line_mode(true);
      label.set_lines(1);
      label.set_xalign(0);
    }

    void updatePlayingStyles(Gtk::ListItem& listItem, rt::TrackField field, bool playing)
    {
      if (auto* const child = listItem.get_child(); child != nullptr)
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

        if (playing)
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
        // displayText() hands back a cached string by pointer for both text-backed
        // and computed fields, so this bind copies straight into the label with no
        // intermediate ustring materialization.
        auto const* const text = row->displayText(field);
        label->set_text(text != nullptr ? *text : Glib::ustring{});
      }
    }

    bool isRowPlaying(TrackRowObject const& row, TrackListModel const& playingModel)
    {
      return row.trackId() == playingModel.playingTrackId();
    }

    // Restyles a (possibly unbound) cell from the model's current playing track.
    // Invoked by the per-cell playing-changed subscription established at setup.
    void restylePlayingFromModel(Gtk::ListItem& listItem, rt::TrackField field, TrackListModel const& playingModel)
    {
      auto const rowPtr = std::dynamic_pointer_cast<TrackRowObject>(listItem.get_item());

      if (rowPtr == nullptr)
      {
        return;
      }

      updatePlayingStyles(listItem, field, isRowPlaying(*rowPtr, playingModel));
    }

    void onTextColumnBind(Glib::RefPtr<Gtk::ListItem> const& listItem,
                          rt::TrackField field,
                          bool editable,
                          TrackListModel const& playingModel)
    {
      auto const itemPtr = listItem->get_item();
      auto const rowPtr = std::dynamic_pointer_cast<TrackRowObject>(itemPtr);

      if (rowPtr == nullptr)
      {
        return;
      }

      if (!editable)
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

          if (label != nullptr && entry != nullptr)
          {
            // The commit handler is wired once at setup, so a (re)bind only
            // refreshes the displayed text.
            auto const* const text = rowPtr->displayText(field);
            auto const& displayText = text != nullptr ? *text : Glib::ustring{};
            label->set_text(displayText);
            entry->set_text(displayText);
          }
        }
      }

      // Status and playing styling for this (re)bind — handles a recycled cell
      // landing on a different row during scroll. In-place playing-state changes
      // (the same cell staying bound while the track switches) are handled by the
      // setup-time subscription to TrackListModel::signalPlayingChanged, because
      // the shared cached row objects make GTK skip the rebind on items_changed.
      updateStatusStyles(listItem, rowPtr);
      updatePlayingStyles(*listItem, field, isRowPlaying(*rowPtr, playingModel));
    }
  } // namespace

  Glib::RefPtr<Gtk::SignalListItemFactory> buildColumnFactory(rt::TrackField field,
                                                              MetadataCommitFn const& commitFn,
                                                              TrackListModel& playingModel)
  {
    auto factoryPtr = Gtk::SignalListItemFactory::create();

    // The field is fixed for this column, so its editability is a constant for the
    // factory's lifetime. Resolve it once here instead of re-scanning the field-UI
    // definition table on every setup and bind.
    auto const* const uiDef = trackFieldUiDefinition(field);
    bool const editable = uiDef != nullptr && canInlineEdit(*uiDef);
    auto* const playingModelRaw = &playingModel;

    factoryPtr->signal_setup().connect(
      [field, editable, commitFn, playingModelRaw](Glib::RefPtr<Gtk::ListItem> const& listItem)
      {
        // Hoisted so the inline-edit commit can be wired once below, after the
        // per-cell bind-data exists; left null for non-editable columns.
        Gtk::Stack* editStack = nullptr;
        Gtk::Entry* editEntry = nullptr;
        Gtk::Label* editLabel = nullptr;

        if (!editable)
        {
          auto* const label = Gtk::make_managed<Gtk::Label>("");
          configureTrackCellLabel(*label);

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
          configureTrackCellLabel(*label);
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

          editStack = stack;
          editEntry = entry;
          editLabel = label;
        }

        // Allocate connection storage once per listItem lifetime, reused across bind/unbind
        auto* const bindData = new CellBindData{};
        listItem->set_data(
          Glib::Quark{"bind-data"}, bindData, [](void* data) { delete static_cast<CellBindData*>(data); });

        // Wire the inline-edit commit once, for the cell's lifetime. The slot
        // resolves the currently-bound row lazily from listItem->get_item() (the
        // entry only commits while the cell it lives in is bound), so a recycled
        // cell needs no reconnect. The raw ListItem is safe to capture: the
        // connection is owned by bindData, owned by the ListItem.
        if (editEntry != nullptr)
        {
          bindData->commitConnection = editEntry->signal_activate().connect(
            [item = listItem.get(), editEntry, editStack, editLabel, field, commitFn]
            {
              auto const rowPtr = std::dynamic_pointer_cast<TrackRowObject>(item->get_item());

              if (rowPtr == nullptr)
              {
                return;
              }

              commitFn(rowPtr, field, editEntry->get_text().raw());
              auto const synced = rowPtr->fieldText(field);
              editLabel->set_text(synced);
              editEntry->set_text(synced);
              editStack->set_visible_child("display");
            });
        }

        // Subscribe once, for the cell's lifetime, to the now-playing signal. The
        // handler captures the raw ListItem (the connection is owned by bindData,
        // owned by the ListItem, so it can never outlive it) to avoid a refcount
        // cycle.
        bindData->playingChangedConnection = playingModelRaw->signalPlayingChanged().connect(
          [item = listItem.get(), field, playingModelRaw] { restylePlayingFromModel(*item, field, *playingModelRaw); });
      });

    factoryPtr->signal_bind().connect([field, editable, playingModelRaw](Glib::RefPtr<Gtk::ListItem> const& listItem)
                                      { onTextColumnBind(listItem, field, editable, *playingModelRaw); });

    return factoryPtr;
  }
} // namespace ao::gtk
