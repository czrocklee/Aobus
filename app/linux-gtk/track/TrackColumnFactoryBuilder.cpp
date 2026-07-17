// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackColumnFactoryBuilder.h"

#include "track/TrackFieldUi.h"
#include "track/TrackListModel.h"
#include "track/TrackRowObject.h"
#include <ao/library/FileManifestLayout.h>
#include <ao/rt/Subscription.h>
#include <ao/rt/TrackField.h>
#include <ao/uimodel/library/presentation/TrackFieldPresentationPolicy.h>
#include <ao/uimodel/library/property/TrackAuthoringSession.h>

#include <gdk/gdkkeysyms.h>
#include <gdkmm/enums.h>
#include <glibmm/main.h>
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
#include <gtkmm/tooltip.h>
#include <pangomm/layout.h>
#include <sigc++/adaptors/track_obj.h>
#include <sigc++/scoped_connection.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace ao::gtk
{
  namespace
  {
    struct CellBindingState final
    {
      // Both connections are established once at setup and held for the cell's
      // lifetime; the scoped_connections auto-disconnect when this struct (owned
      // by the ListItem) is destroyed. Neither is touched by bind/unbind: the
      // slots resolve the currently-bound row lazily from listItem->get_item(),
      // so a recycled cell needs no reconnect.
      sigc::scoped_connection commitConnection;         // inline-edit commit (Enter)
      sigc::scoped_connection playingChangedConnection; // now-playing highlight
      std::unique_ptr<uimodel::TrackAuthoringSession> editSessionPtr;
      rt::Subscription editSessionStateSubscription;
      std::uint64_t editSessionGeneration = 0;

      void clearEditSession()
      {
        ++editSessionGeneration;
        editSessionStateSubscription.reset();
        editSessionPtr.reset();
      }
    };

    bool isLabelTextEllipsized(Gtk::Label const& label)
    {
      auto const layoutPtr = label.get_layout();
      return layoutPtr != nullptr && layoutPtr->is_ellipsized();
    }

    void installEllipsizedTextTooltip(Gtk::Label& label)
    {
      label.set_has_tooltip();
      label.signal_query_tooltip().connect(
        [&label](std::int32_t /*x*/,
                 std::int32_t /*y*/,
                 bool /*keyboardTooltip*/,
                 Glib::RefPtr<Gtk::Tooltip> const& tooltipPtr)
        {
          if (!isLabelTextEllipsized(label))
          {
            return false;
          }

          auto const text = label.get_text();

          if (text.empty())
          {
            return false;
          }

          if (tooltipPtr != nullptr)
          {
            tooltipPtr->set_text(text);
          }

          return true;
        },
        false);
    }

    void configureTrackCellLabel(Gtk::Label& label, rt::TrackField field)
    {
      if (uimodel::trackFieldColumnAlignment(field) == uimodel::TrackColumnAlignment::End)
      {
        label.set_halign(Gtk::Align::END);
        label.set_xalign(1.0F);
        label.set_margin_end(8);
      }
      else
      {
        label.set_halign(Gtk::Align::START);
        label.set_xalign(0.0F);
      }

      label.set_ellipsize(Pango::EllipsizeMode::END);
      label.set_single_line_mode(true);
      label.set_lines(1);
      installEllipsizedTextTooltip(label);
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

    void updateStatusStyles(Glib::RefPtr<Gtk::ListItem> const& listItemPtr, Glib::RefPtr<TrackRowObject> const& rowPtr)
    {
      if (auto* const child = listItemPtr->get_child(); child != nullptr)
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

        if (rowPtr->status() == library::FileStatus::Missing)
        {
          rowWidget->add_css_class("ao-missing-row");
        }
        else
        {
          rowWidget->remove_css_class("ao-missing-row");
        }
      }
    }

    void handleTextColumnBindStatic(Glib::RefPtr<Gtk::ListItem> const& listItemPtr,
                                    rt::TrackField field,
                                    Glib::RefPtr<TrackRowObject> const& rowPtr)
    {
      auto* const label = dynamic_cast<Gtk::Label*>(listItemPtr->get_child());

      if (label != nullptr)
      {
        // displayText() hands back a cached string by pointer for both text-backed
        // and computed fields, so this bind copies straight into the label with no
        // intermediate ustring materialization.
        auto const* const text = rowPtr->displayText(field);
        auto const& displayText = text != nullptr ? *text : Glib::ustring{};
        label->set_text(displayText);
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

    void handleTextColumnBind(Glib::RefPtr<Gtk::ListItem> const& listItemPtr,
                              rt::TrackField field,
                              bool editable,
                              TrackListModel const& playingModel)
    {
      auto const itemPtr = listItemPtr->get_item();
      auto const rowPtr = std::dynamic_pointer_cast<TrackRowObject>(itemPtr);

      if (rowPtr == nullptr)
      {
        return;
      }

      if (!editable)
      {
        handleTextColumnBindStatic(listItemPtr, field, rowPtr);
      }
      else
      {
        auto* const stack = dynamic_cast<Gtk::Stack*>(listItemPtr->get_child());

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
      updateStatusStyles(listItemPtr, rowPtr);
      updatePlayingStyles(*listItemPtr, field, isRowPlaying(*rowPtr, playingModel));
    }

    void closeInlineEditor(Gtk::Stack& stack, CellBindingState& bindingState)
    {
      bindingState.clearEditSession();
      stack.set_visible_child("display");
    }

    void handleEditSessionState(Gtk::ListItem& item,
                                Gtk::Stack& stack,
                                CellBindingState& bindingState,
                                std::uint64_t editSessionGeneration,
                                uimodel::TrackAuthoringSessionState state)
    {
      if (state != uimodel::TrackAuthoringSessionState::Stale && state != uimodel::TrackAuthoringSessionState::Rejected)
      {
        return;
      }

      // Defer teardown until the state signal has unwound: the subscription
      // being reset owns this callback. Tracking the ListItem prevents the
      // deferred raw-state access after cell destruction, while the generation
      // protects a replacement edit session installed before the idle callback.
      Glib::signal_idle().connect_once(sigc::track_object(
        [stackRaw = &stack, bindingStateRaw = &bindingState, editSessionGeneration]
        {
          if (bindingStateRaw->editSessionGeneration != editSessionGeneration)
          {
            return;
          }

          closeInlineEditor(*stackRaw, *bindingStateRaw);
        },
        item));
    }

    void beginInlineEdit(Gtk::ListItem& item,
                         Gtk::Stack& stack,
                         CellBindingState& bindingState,
                         MetadataEditSessionFn const& beginEditSession)
    {
      bindingState.clearEditSession();
      auto const rowPtr = std::dynamic_pointer_cast<TrackRowObject>(item.get_item());

      if (rowPtr == nullptr || !beginEditSession)
      {
        stack.set_visible_child("display");
        return;
      }

      auto sessionResult = beginEditSession(rowPtr);

      if (!sessionResult)
      {
        stack.set_visible_child("display");
        return;
      }

      bindingState.editSessionPtr = std::move(*sessionResult);
      auto const editSessionGeneration = bindingState.editSessionGeneration;
      bindingState.editSessionStateSubscription = bindingState.editSessionPtr->onStateChanged(
        [itemRaw = &item, stackRaw = &stack, bindingStateRaw = &bindingState, editSessionGeneration](
          uimodel::TrackAuthoringSessionState state)
        { handleEditSessionState(*itemRaw, *stackRaw, *bindingStateRaw, editSessionGeneration, state); });
    }

    void installInlineEditControllers(Gtk::ListItem& item,
                                      Gtk::Stack& stack,
                                      Gtk::Entry& entry,
                                      CellBindingState& bindingState,
                                      MetadataEditSessionFn const& beginEditSession)
    {
      auto const keyControllerPtr = Gtk::EventControllerKey::create();
      keyControllerPtr->signal_key_pressed().connect(
        [stackRaw = &stack, bindingStateRaw = &bindingState](
          std::uint32_t keyval, std::uint32_t /*keycode*/, Gdk::ModifierType /*state*/)
        {
          if (keyval != GDK_KEY_Escape)
          {
            return false;
          }

          closeInlineEditor(*stackRaw, *bindingStateRaw);
          return true;
        },
        false);
      entry.add_controller(keyControllerPtr);

      auto const focusControllerPtr = Gtk::EventControllerFocus::create();
      focusControllerPtr->signal_enter().connect(
        [itemRaw = &item, stackRaw = &stack, bindingStateRaw = &bindingState, beginEditSession]
        { beginInlineEdit(*itemRaw, *stackRaw, *bindingStateRaw, beginEditSession); });
      focusControllerPtr->signal_leave().connect([stackRaw = &stack, bindingStateRaw = &bindingState]
                                                 { closeInlineEditor(*stackRaw, *bindingStateRaw); });
      entry.add_controller(focusControllerPtr);
    }

    bool editSessionTargetsRow(CellBindingState const& bindingState, TrackRowObject const& row)
    {
      return bindingState.editSessionPtr != nullptr && bindingState.editSessionPtr->targetIds().size() == 1 &&
             bindingState.editSessionPtr->targetIds().front() == row.trackId();
    }

    void commitInlineEdit(Gtk::ListItem& item,
                          Gtk::Stack& stack,
                          Gtk::Entry& entry,
                          Gtk::Label& label,
                          CellBindingState& bindingState,
                          rt::TrackField field,
                          MetadataCommitFn const& commitFn)
    {
      auto const rowPtr = std::dynamic_pointer_cast<TrackRowObject>(item.get_item());

      if (rowPtr == nullptr || !editSessionTargetsRow(bindingState, *rowPtr))
      {
        closeInlineEditor(stack, bindingState);
        return;
      }

      commitFn(rowPtr, field, entry.get_text().raw(), *bindingState.editSessionPtr);
      auto const synced = rowPtr->fieldText(field);
      label.set_text(synced);
      entry.set_text(synced);
      closeInlineEditor(stack, bindingState);
    }

    void setupStaticCell(Gtk::ListItem& item, rt::TrackField field)
    {
      auto* const label = Gtk::make_managed<Gtk::Label>("");
      configureTrackCellLabel(*label, field);

      if (field == rt::TrackField::Title)
      {
        label->add_css_class("ao-track-title-cell");
      }

      if (field == rt::TrackField::Duration || field == rt::TrackField::Year)
      {
        label->add_css_class("dim-label");
      }

      if (field == rt::TrackField::Tags)
      {
        label->add_css_class(kTagsCellCssClass);
      }

      item.set_child(*label);
    }

    void setupEditableCell(Gtk::ListItem& item,
                           CellBindingState& bindingState,
                           rt::TrackField field,
                           MetadataEditSessionFn const& beginEditSession,
                           MetadataCommitFn const& commitFn)
    {
      auto* const stack = Gtk::make_managed<Gtk::Stack>();
      stack->set_vhomogeneous(false);
      stack->set_hhomogeneous(false);
      stack->set_transition_type(Gtk::StackTransitionType::NONE);
      stack->add_css_class("ao-inline-editor-stack");

      auto* const label = Gtk::make_managed<Gtk::Label>("");
      label->add_css_class("ao-inline-editor-label");
      configureTrackCellLabel(*label, field);

      if (field == rt::TrackField::Title)
      {
        label->add_css_class("ao-track-title-cell");
      }

      stack->add(*label, "display");

      auto* const entry = Gtk::make_managed<Gtk::Entry>();
      entry->add_css_class("ao-inline-editor-entry");
      installInlineEditControllers(item, *stack, *entry, bindingState, beginEditSession);
      stack->add(*entry, "edit");
      item.set_child(*stack);

      bindingState.commitConnection = entry->signal_activate().connect(
        [itemRaw = &item, stack, entry, label, bindingStateRaw = &bindingState, field, commitFn]
        { commitInlineEdit(*itemRaw, *stack, *entry, *label, *bindingStateRaw, field, commitFn); });
    }

    CellBindingState& installCellBindingState(Gtk::ListItem& item)
    {
      auto* const bindingState = new CellBindingState{};
      item.set_data(Glib::Quark{"cell-binding-state"},
                    bindingState,
                    [](void* rawBindingState) { delete static_cast<CellBindingState*>(rawBindingState); });
      return *bindingState;
    }

    void setupTextColumnCell(Gtk::ListItem& item,
                             rt::TrackField field,
                             bool editable,
                             MetadataEditSessionFn const& beginEditSession,
                             MetadataCommitFn const& commitFn,
                             TrackListModel& playingModel)
    {
      auto& bindingState = installCellBindingState(item);

      if (editable)
      {
        setupEditableCell(item, bindingState, field, beginEditSession, commitFn);
      }
      else
      {
        setupStaticCell(item, field);
      }

      // The handler captures the raw ListItem because the connection is owned by
      // bindingState, which is itself owned by the ListItem.
      bindingState.playingChangedConnection =
        playingModel.signalPlayingChanged().connect([itemRaw = &item, field, playingModelRaw = &playingModel]
                                                    { restylePlayingFromModel(*itemRaw, field, *playingModelRaw); });
    }
  } // namespace

  Glib::RefPtr<Gtk::SignalListItemFactory> buildColumnFactory(rt::TrackField field,
                                                              MetadataEditSessionFn const& beginEditSession,
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
      [field, editable, beginEditSession, commitFn, playingModelRaw](Glib::RefPtr<Gtk::ListItem> const& listItemPtr)
      { setupTextColumnCell(*listItemPtr, field, editable, beginEditSession, commitFn, *playingModelRaw); });

    factoryPtr->signal_bind().connect([field, editable, playingModelRaw](Glib::RefPtr<Gtk::ListItem> const& listItemPtr)
                                      { handleTextColumnBind(listItemPtr, field, editable, *playingModelRaw); });

    return factoryPtr;
  }
} // namespace ao::gtk
