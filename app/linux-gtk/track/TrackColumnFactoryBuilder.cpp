// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackColumnFactoryBuilder.h"

#include "track/TrackFieldUi.h"
#include "track/TrackListItem.h"
#include "track/TrackListModel.h"
#include "track/TrackRowBinding.h"
#include "track/TrackRowObject.h"
#include "track/TrackRowPathDemand.h"
#include <ao/async/Subscription.h>
#include <ao/library/FileStatus.h>
#include <ao/rt/TrackField.h>
#include <ao/uimodel/library/presentation/TrackColumnDefaults.h>
#include <ao/uimodel/library/track/TrackAuthoringSessions.h>

#include <gdk/gdkkeysyms.h>
#include <gdkmm/enums.h>
#include <glib-object.h>
#include <glibmm/main.h>
#include <glibmm/quark.h>
#include <glibmm/refptr.h>
#include <glibmm/ustring.h>
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
#include <optional>
#include <string>
#include <utility>

namespace ao::gtk
{
  namespace
  {
    struct CellBindingState final
    {
      // The ListItem owns these widgets, created once at setup. Static cells
      // have only the display label.
      Gtk::Label* displayLabel = nullptr;
      Gtk::Stack* editorStack = nullptr;
      Gtk::Entry* editorEntry = nullptr;

      // Setup-time handlers use the binding current at invocation.
      sigc::scoped_connection commitConnection;
      sigc::scoped_connection playingChangedConnection;
      std::optional<uimodel::TrackAuthoringSession> optEditSession;
      async::Subscription editSessionInvalidatedSubscription;
      sigc::scoped_connection editSessionInvalidationIdleConnection;
      // Native list items carry only identity; keep the materialized fields
      // alive for this binding and disconnect its signal before releasing it.
      Glib::RefPtr<TrackRowObject> boundRowPtr;
      sigc::scoped_connection fieldsChangedConnection;

      void clearEditSession()
      {
        editSessionInvalidationIdleConnection.disconnect();
        editSessionInvalidatedSubscription.reset();
        optEditSession.reset();
      }
    };

    Glib::Quark const& cellBindingStateKey()
    {
      static auto const kKey = Glib::Quark{"cell-binding-state"};
      return kKey;
    }

    CellBindingState& cellBindingState(Gtk::ListItem& item)
    {
      return *static_cast<CellBindingState*>(item.get_data(cellBindingStateKey()));
    }

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
      if (uimodel::trackColumnDefaults(field).alignment == uimodel::TrackColumnAlignment::End)
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

    void updateStatusStyles(Gtk::ListItem& listItem, bool missing)
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

        if (missing)
        {
          rowWidget->add_css_class("ao-missing-row");
        }
        else
        {
          rowWidget->remove_css_class("ao-missing-row");
        }
      }
    }

    void refreshCellDisplay(Gtk::ListItem& item,
                            CellBindingState const& bindingState,
                            rt::TrackField field,
                            TrackRowObject const* row)
    {
      static Glib::ustring const kEmpty;
      auto const* const text = row != nullptr ? row->displayText(field) : nullptr;
      auto const& displayText = text != nullptr ? *text : kEmpty;
      bindingState.displayLabel->set_text(displayText);

      // The label tracks publications even while the visible entry holds a draft.
      if (auto* const stack = bindingState.editorStack;
          stack != nullptr && stack->get_visible_child() != bindingState.editorEntry)
      {
        bindingState.editorEntry->set_text(displayText);
      }

      updateStatusStyles(item, row != nullptr && row->status() == library::FileStatus::Missing);
    }

    void restylePlayingFromModel(Gtk::ListItem& listItem, rt::TrackField field, TrackListModel const& model)
    {
      auto const trackItemPtr = std::dynamic_pointer_cast<TrackListItem>(listItem.get_item());

      if (trackItemPtr != nullptr)
      {
        updatePlayingStyles(listItem, field, trackItemPtr->trackId() == model.playingTrackId());
      }
    }

    void handleTextColumnBind(Gtk::ListItem& item,
                              rt::TrackField field,
                              TrackListModel const& model,
                              TrackRowPathDemand const pathDemand)
    {
      auto const trackItemPtr = std::dynamic_pointer_cast<TrackListItem>(item.get_item());

      if (trackItemPtr == nullptr)
      {
        return;
      }

      auto& bindingState = cellBindingState(item);
      auto const trackId = trackItemPtr->trackId();
      auto const isFilePath = field == rt::TrackField::FilePath;
      auto const rowPtr = model.trackRow(trackId, pathDemand);
      // GTK unbinds before reuse; unbind retires the prior field observer and row together.
      bindingState.boundRowPtr = rowPtr;

      refreshCellDisplay(item, bindingState, field, rowPtr.get());
      updatePlayingStyles(item, field, trackId == model.playingTrackId());

      if (auto* const child = item.get_child(); child != nullptr)
      {
        ::g_object_set_data(
          G_OBJECT(child->gobj()), kBoundTrackIdDataKey, GUINT_TO_POINTER(static_cast<guint>(trackId.raw())));
      }

      if (rowPtr != nullptr)
      {
        auto& changed = isFilePath ? rowPtr->signalFilePathChanged() : rowPtr->signalFieldsChanged();
        bindingState.fieldsChangedConnection =
          changed.connect([itemRaw = &item, bindingStateRaw = &bindingState, field, rowRaw = rowPtr.get()]
                          { refreshCellDisplay(*itemRaw, *bindingStateRaw, field, rowRaw); });
      }
    }

    void closeInlineEditor(CellBindingState& bindingState)
    {
      bindingState.clearEditSession();

      if (auto* const stack = bindingState.editorStack; stack != nullptr)
      {
        bindingState.editorEntry->set_text(bindingState.displayLabel->get_text());
        stack->set_visible_child(*bindingState.displayLabel);
      }
    }

    void handleTextColumnUnbind(Gtk::ListItem& item)
    {
      auto& bindingState = cellBindingState(item);
      bindingState.fieldsChangedConnection.disconnect();

      if (auto* const child = item.get_child(); child != nullptr)
      {
        ::g_object_set_data(G_OBJECT(child->gobj()), kBoundTrackIdDataKey, nullptr);
      }

      // GTK may unbind a cell without delivering focus leave to its entry.
      closeInlineEditor(bindingState);
      bindingState.boundRowPtr.reset();
    }

    void handleEditSessionInvalidated(Gtk::ListItem& item, CellBindingState& bindingState)
    {
      // Defer teardown until the invalidation callback has unwound: the subscription
      // being reset owns this callback. Tracking the ListItem prevents deferred
      // raw-state access after cell destruction; replacing the edit session
      // disconnects this exact idle callback.
      bindingState.editSessionInvalidationIdleConnection = Glib::signal_idle().connect(sigc::track_object(
        [bindingStateRaw = &bindingState]
        {
          closeInlineEditor(*bindingStateRaw);
          return false;
        },
        item));
    }

    Glib::RefPtr<TrackRowObject> currentBoundRow(Gtk::ListItem& item, CellBindingState const& bindingState)
    {
      auto const trackItemPtr = std::dynamic_pointer_cast<TrackListItem>(item.get_item());
      auto const& rowPtr = bindingState.boundRowPtr;

      if (trackItemPtr != nullptr && rowPtr != nullptr && trackItemPtr->trackId() == rowPtr->trackId())
      {
        return rowPtr;
      }

      return nullptr;
    }

    bool isEditSessionTargetRow(uimodel::TrackAuthoringSession const& editSession, TrackRowObject const& row)
    {
      auto const ids = editSession.targetIds();
      return ids.size() == 1 && ids.front() == row.trackId();
    }

    void beginInlineEdit(Gtk::ListItem& item,
                         CellBindingState& bindingState,
                         MetadataEditSessionFn const& beginEditSession)
    {
      bindingState.clearEditSession();
      auto const rowPtr = currentBoundRow(item, bindingState);

      if (rowPtr == nullptr || !beginEditSession)
      {
        closeInlineEditor(bindingState);
        return;
      }

      auto sessionRes = beginEditSession(rowPtr);

      if (!sessionRes || !isEditSessionTargetRow(*sessionRes, *rowPtr))
      {
        closeInlineEditor(bindingState);
        return;
      }

      bindingState.optEditSession.emplace(std::move(*sessionRes));
      bindingState.editSessionInvalidatedSubscription =
        bindingState.optEditSession->onInvalidated([itemRaw = &item, bindingStateRaw = &bindingState] noexcept
                                                   { handleEditSessionInvalidated(*itemRaw, *bindingStateRaw); });
    }

    void installInlineEditControllers(Gtk::ListItem& item,
                                      Gtk::Entry& entry,
                                      CellBindingState& bindingState,
                                      MetadataEditSessionFn const& beginEditSession)
    {
      auto const keyControllerPtr = Gtk::EventControllerKey::create();
      keyControllerPtr->signal_key_pressed().connect(
        [bindingStateRaw = &bindingState](std::uint32_t keyval, std::uint32_t /*keycode*/, Gdk::ModifierType /*state*/)
        {
          if (keyval != GDK_KEY_Escape)
          {
            return false;
          }

          closeInlineEditor(*bindingStateRaw);
          return true;
        },
        false);
      entry.add_controller(keyControllerPtr);

      auto const focusControllerPtr = Gtk::EventControllerFocus::create();
      focusControllerPtr->signal_enter().connect([itemRaw = &item, bindingStateRaw = &bindingState, beginEditSession]
                                                 { beginInlineEdit(*itemRaw, *bindingStateRaw, beginEditSession); });
      focusControllerPtr->signal_leave().connect([bindingStateRaw = &bindingState]
                                                 { closeInlineEditor(*bindingStateRaw); });
      entry.add_controller(focusControllerPtr);
    }

    void commitInlineEdit(Gtk::ListItem& item,
                          CellBindingState& bindingState,
                          rt::TrackField field,
                          MetadataCommitFn const& commitFn)
    {
      auto const rowPtr = currentBoundRow(item, bindingState);

      if (rowPtr == nullptr || !bindingState.optEditSession ||
          !isEditSessionTargetRow(*bindingState.optEditSession, *rowPtr))
      {
        closeInlineEditor(bindingState);
        return;
      }

      auto draft = bindingState.editorEntry->get_text().raw();
      auto session = std::move(*bindingState.optEditSession);
      closeInlineEditor(bindingState);
      commitFn(rowPtr, field, std::move(draft), session);
    }

    void setupStaticCell(Gtk::ListItem& item, CellBindingState& bindingState, rt::TrackField field)
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
      bindingState.displayLabel = label;
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
      bindingState.displayLabel = label;
      bindingState.editorStack = stack;
      bindingState.editorEntry = entry;
      installInlineEditControllers(item, *entry, bindingState, beginEditSession);
      stack->add(*entry, "edit");
      item.set_child(*stack);

      bindingState.commitConnection =
        entry->signal_activate().connect([itemRaw = &item, bindingStateRaw = &bindingState, field, commitFn]
                                         { commitInlineEdit(*itemRaw, *bindingStateRaw, field, commitFn); });
    }

    CellBindingState& installCellBindingState(Gtk::ListItem& item)
    {
      auto* const bindingState = new CellBindingState{};
      item.set_data(cellBindingStateKey(),
                    bindingState,
                    [](void* rawBindingState) { delete static_cast<CellBindingState*>(rawBindingState); });
      return *bindingState;
    }

    void setupTextColumnCell(Gtk::ListItem& item,
                             rt::TrackField field,
                             bool editable,
                             MetadataEditSessionFn const& beginEditSession,
                             MetadataCommitFn const& commitFn,
                             TrackListModel& model)
    {
      auto& bindingState = installCellBindingState(item);

      if (editable)
      {
        setupEditableCell(item, bindingState, field, beginEditSession, commitFn);
      }
      else
      {
        setupStaticCell(item, bindingState, field);
      }

      // This observer spans cell reuse and reads the item currently bound.
      // The ListItem owns the state, so the connection cannot outlive itemRaw.
      bindingState.playingChangedConnection = model.signalPlayingChanged().connect(
        [itemRaw = &item, field, modelRaw = &model] { restylePlayingFromModel(*itemRaw, field, *modelRaw); });
    }
  } // namespace

  Glib::RefPtr<Gtk::SignalListItemFactory> buildColumnFactory(rt::TrackField field,
                                                              MetadataEditSessionFn const& beginEditSession,
                                                              MetadataCommitFn const& commitFn,
                                                              TrackListModel& model,
                                                              TrackRowPathDemand const viewPathDemand)
  {
    auto factoryPtr = Gtk::SignalListItemFactory::create();

    // The field is fixed for this column, so its editability is a constant for the
    // factory's lifetime. Resolve it once here instead of re-scanning the field-UI
    // definition table on every setup and bind.
    auto const* const uiDef = trackFieldUiDefinition(field);
    bool const editable = uiDef != nullptr && canInlineEdit(*uiDef);
    auto* const modelRaw = &model;

    factoryPtr->signal_setup().connect(
      [field, editable, beginEditSession, commitFn, modelRaw](Glib::RefPtr<Gtk::ListItem> const& listItemPtr)
      { setupTextColumnCell(*listItemPtr, field, editable, beginEditSession, commitFn, *modelRaw); });

    auto const pathDemand = field == rt::TrackField::FilePath ? TrackRowPathDemand::Required : viewPathDemand;
    factoryPtr->signal_bind().connect([field, modelRaw, pathDemand](Glib::RefPtr<Gtk::ListItem> const& listItemPtr)
                                      { handleTextColumnBind(*listItemPtr, field, *modelRaw, pathDemand); });
    factoryPtr->signal_unbind().connect([](Glib::RefPtr<Gtk::ListItem> const& listItemPtr)
                                        { handleTextColumnUnbind(*listItemPtr); });

    return factoryPtr;
  }
} // namespace ao::gtk
