// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "preferences/ShortcutEditorWidget.h"

#include "app/AppDialog.h"
#include "app/GtkAccelTranslator.h"
#include <ao/uimodel/input/KeyChord.h>
#include <ao/uimodel/input/KeymapModel.h>
#include <ao/uimodel/layout/action/LayoutActionCapabilities.h>
#include <ao/uimodel/layout/action/LayoutActionCatalog.h>
#include <ao/uimodel/layout/action/LayoutActionDescriptor.h>

#include <gdk/gdkkeysyms.h>
#include <gdkmm/enums.h>
#include <glibmm/main.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/dialog.h>
#include <gtkmm/enums.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/label.h>
#include <gtkmm/object.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/window.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ao::gtk
{
  namespace
  {
    using uimodel::LayoutActionCapability;

    constexpr auto kContentMargin = 12;
    constexpr auto kWarningMarginBottom = 6;
    constexpr auto kCategoryMarginTop = 12;

    bool isShortcutEligible(uimodel::LayoutActionDescriptor const& desc)
    {
      // A global accelerator fires with no widget anchor and no surface to host a popover, so
      // actions that require an anchor or present a menu cannot be driven by one.
      return !desc.capabilities.has(LayoutActionCapability::RequiresAnchor) &&
             !desc.capabilities.has(LayoutActionCapability::PresentsMenu);
    }

    Gdk::ModifierType accelMods(Gdk::ModifierType state)
    {
      return state & (Gdk::ModifierType::CONTROL_MASK | Gdk::ModifierType::SHIFT_MASK | Gdk::ModifierType::ALT_MASK |
                      Gdk::ModifierType::SUPER_MASK);
    }
  } // namespace

  ShortcutEditorWidget::ShortcutEditorWidget(uimodel::LayoutActionCatalog const& catalog,
                                             uimodel::KeymapModel keymap,
                                             ChangedCallback onChanged,
                                             Gtk::Window& hostForDialogs)
    : Gtk::Box{Gtk::Orientation::VERTICAL, 8}
    , _hostForDialogs{hostForDialogs}
    , _keymap{std::move(keymap)}
    , _onChanged{std::move(onChanged)}
  {
    // Default reassignment prompt: a modal AppDialog parented to the injected host. Tests replace
    // this via setConflictConfirmer() so the decision is driven synchronously without a real dialog.
    _conflictConfirmer =
      [this](std::string const& ownerLabel, std::string const& chordText, std::function<void(bool)> respond)
    {
      AppDialog::presentMessage(
        _hostForDialogs,
        "Reassign shortcut?",
        chordText + " is already assigned to \"" + ownerLabel + "\". Reassign it to this action?",
        {AppDialogAction{
           .label = "Cancel", .responseId = Gtk::ResponseType::CANCEL, .role = AppDialogActionRole::Cancel},
         AppDialogAction{
           .label = "Reassign", .responseId = Gtk::ResponseType::OK, .role = AppDialogActionRole::Primary}},
        Gtk::ResponseType::OK,
        [respond = std::move(respond)](std::int32_t const responseId)
        { respond(responseId == Gtk::ResponseType::OK); });
    };

    for (auto const& desc : catalog.descriptors())
    {
      if (!isShortcutEligible(desc))
      {
        continue;
      }

      _actions.push_back({.id = desc.id,
                          .label = desc.label.empty() ? desc.id : desc.label,
                          .category = desc.category.empty() ? "Other" : desc.category});
      _editableActionIds.push_back(desc.id);
    }

    set_margin(kContentMargin);
    _unmapConn = signal_unmap().connect([this] { closeCapture(); });

    auto* const header = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    auto* const title = Gtk::make_managed<Gtk::Label>("Customize keyboard shortcuts");
    title->set_xalign(0.0F);
    title->set_hexpand(true);
    title->add_css_class("title-4");
    header->append(*title);

    auto* const resetAllButton = Gtk::make_managed<Gtk::Button>("Reset All");
    resetAllButton->set_tooltip_text("Reset every shortcut to its default");
    resetAllButton->signal_clicked().connect([this] { resetAll(); });
    header->append(*resetAllButton);
    append(*header);

    auto* const scroller = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroller->set_vexpand(true);
    scroller->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);

    auto* const list = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
    _listBox = list;
    scroller->set_child(*list);
    append(*scroller);

    rebuild();
  }

  ShortcutEditorWidget::~ShortcutEditorWidget()
  {
    *_aliveTokenPtr = false;

    // Cancel a still-pending capture teardown and mark delayed conflict responses as stale.
    _captureCloseConn.disconnect();
    _unmapConn.disconnect();
  }

  bool ShortcutEditorWidget::bindChord(std::string const& actionId, uimodel::KeyChord const& chord)
  {
    if (!chord.isValid())
    {
      return false;
    }

    bool changed = false;

    // Transfer the chord away from any other action currently holding it, so the editor never
    // produces an ambiguous (conflicting) effective keymap.
    for (;;)
    {
      auto const optOwner = _keymap.actionFor(chord);

      if (!optOwner || *optOwner == actionId)
      {
        break;
      }

      _keymap.unbind(*optOwner, chord);
      changed = true;
    }

    if (_keymap.bind(actionId, chord))
    {
      changed = true;
    }

    if (changed)
    {
      commit();
    }

    return changed;
  }

  std::optional<std::string> ShortcutEditorWidget::conflictingOwner(std::string const& actionId,
                                                                    uimodel::KeyChord const& chord) const
  {
    if (auto optOwner = _keymap.actionFor(chord); optOwner && *optOwner != actionId)
    {
      return optOwner;
    }

    return std::nullopt;
  }

  void ShortcutEditorWidget::requestBind(std::string const& actionId, uimodel::KeyChord const& chord)
  {
    auto const optOwner = conflictingOwner(actionId, chord);

    if (!optOwner)
    {
      bindChord(actionId, chord);
      return;
    }

    _conflictConfirmer(labelFor(*optOwner),
                       chord.toString(),
                       [this, aliveTokenPtr = _aliveTokenPtr, actionId, chord](bool accepted)
                       {
                         if (accepted && *aliveTokenPtr)
                         {
                           bindChord(actionId, chord);
                         }
                       });
  }

  std::string ShortcutEditorWidget::labelFor(std::string const& actionId) const
  {
    for (auto const& action : _actions)
    {
      if (action.id == actionId)
      {
        return action.label;
      }
    }

    return actionId;
  }

  bool ShortcutEditorWidget::unbindChord(std::string const& actionId, uimodel::KeyChord const& chord)
  {
    if (!_keymap.unbind(actionId, chord))
    {
      return false;
    }

    commit();
    return true;
  }

  void ShortcutEditorWidget::resetAction(std::string const& actionId)
  {
    _keymap.resetToDefault(actionId);
    commit();
  }

  void ShortcutEditorWidget::resetAll()
  {
    _keymap.resetAllToDefault();
    commit();
  }

  void ShortcutEditorWidget::commit()
  {
    rebuild();

    if (_onChanged)
    {
      _onChanged(_keymap);
    }
  }

  void ShortcutEditorWidget::rebuild()
  {
    while (auto* const child = _listBox->get_first_child())
    {
      _listBox->remove(*child);
    }

    if (auto const conflicts = _keymap.conflicts(); !conflicts.empty())
    {
      auto text = std::string{"Conflicting shortcuts: "};

      for (std::size_t i = std::size_t{0}; i < conflicts.size(); ++i)
      {
        text += (i == 0 ? "" : "; ");
        text += conflicts[i].chord.toString();
      }

      auto* const warning = Gtk::make_managed<Gtk::Label>(text);
      warning->set_xalign(0.0F);
      warning->set_wrap(true);
      warning->add_css_class("error");
      warning->set_margin_bottom(kWarningMarginBottom);
      _listBox->append(*warning);
    }

    auto currentCategory = std::string{};

    for (auto const& action : _actions)
    {
      if (action.category != currentCategory)
      {
        currentCategory = action.category;
        auto* const categoryLabel = Gtk::make_managed<Gtk::Label>(currentCategory);
        categoryLabel->set_xalign(0.0F);
        categoryLabel->add_css_class("heading");
        categoryLabel->set_margin_top(kCategoryMarginTop);
        categoryLabel->set_margin_bottom(4);
        _listBox->append(*categoryLabel);
      }

      _listBox->append(buildActionRow(action));
    }
  }

  Gtk::Widget& ShortcutEditorWidget::buildActionRow(EditableAction const& action)
  {
    auto* const row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    row->set_margin_top(2);
    row->set_margin_bottom(2);

    auto* const nameLabel = Gtk::make_managed<Gtk::Label>(action.label);
    nameLabel->set_xalign(0.0F);
    nameLabel->set_hexpand(true);
    row->append(*nameLabel);

    if (auto const chords = _keymap.chordsFor(action.id); chords.empty())
    {
      auto* const unassigned = Gtk::make_managed<Gtk::Label>("Unassigned");
      unassigned->add_css_class("dim-label");
      row->append(*unassigned);
    }
    else
    {
      for (auto const& chord : chords)
      {
        auto* const chip = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 2);
        chip->add_css_class("linked");

        auto* const chordLabel = Gtk::make_managed<Gtk::Label>(chord.toString());
        chordLabel->set_margin_start(4);
        chip->append(*chordLabel);

        auto* const removeButton = Gtk::make_managed<Gtk::Button>("✕");
        removeButton->add_css_class("flat");
        removeButton->set_tooltip_text("Remove " + chord.toString());
        removeButton->signal_clicked().connect([this, id = action.id, chord] { unbindChord(id, chord); });
        chip->append(*removeButton);

        row->append(*chip);
      }
    }

    auto* const addButton = Gtk::make_managed<Gtk::Button>("Add…");
    addButton->set_tooltip_text("Record a new shortcut for this action");
    addButton->signal_clicked().connect([this, id = action.id] { beginCapture(id); });
    row->append(*addButton);

    auto* const resetButton = Gtk::make_managed<Gtk::Button>("Reset");
    resetButton->set_tooltip_text("Reset this action to its default");
    resetButton->signal_clicked().connect([this, id = action.id] { resetAction(id); });
    row->append(*resetButton);

    return *row;
  }

  void ShortcutEditorWidget::beginCapture(std::string actionId)
  {
    closeCapture();

    auto capturePtr = std::make_unique<Gtk::Window>();
    capturePtr->set_title("Set Shortcut");
    capturePtr->set_transient_for(_hostForDialogs);
    capturePtr->set_modal(true);
    capturePtr->set_resizable(false);

    auto* const box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    box->set_margin(24);

    auto* const prompt = Gtk::make_managed<Gtk::Label>("Press the new shortcut combination.");
    box->append(*prompt);

    auto* const hint = Gtk::make_managed<Gtk::Label>("Esc to cancel");
    hint->add_css_class("dim-label");
    box->append(*hint);

    capturePtr->set_child(*box);

    auto keyControllerPtr = Gtk::EventControllerKey::create();
    keyControllerPtr->signal_key_pressed().connect(
      [this, actionId = std::move(actionId)](guint keyval, guint /*keycode*/, Gdk::ModifierType state) -> bool
      {
        if (keyval == GDK_KEY_Escape && accelMods(state) == Gdk::ModifierType{})
        {
          closeCapture();
          return true;
        }

        if (auto const optChord = fromGtkKeyval(keyval, state); optChord)
        {
          // Close the capture popup first so the (possible) reassignment dialog is not stacked
          // under a modal grab, then route through requestBind() for conflict confirmation.
          closeCapture();
          requestBind(actionId, *optChord);
        }

        // Swallow everything else (lone modifiers, unmappable keys) and keep waiting.
        return true;
      },
      false);
    capturePtr->add_controller(keyControllerPtr);

    capturePtr->present();
    _captureWindowPtr = std::move(capturePtr);
  }

  void ShortcutEditorWidget::closeCapture()
  {
    if (!_captureWindowPtr)
    {
      return;
    }

    auto capturePtr = std::shared_ptr<Gtk::Window>{_captureWindowPtr.release()};
    capturePtr->set_visible(false);

    // Defer destruction: closeCapture() runs inside the capture window's own key controller, so
    // the window (and that controller) must outlive the current event dispatch. The connection is
    // retained so the destructor can cancel a teardown that is still pending.
    _captureCloseConn.disconnect();
    _captureCloseConn = Glib::signal_idle().connect(
      [capturePtr = std::move(capturePtr)]
      {
        return false; // one-shot
      });
  }
} // namespace ao::gtk
