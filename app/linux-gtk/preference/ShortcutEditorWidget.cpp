// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "preference/ShortcutEditorWidget.h"

#include "app/AppDialog.h"
#include "app/GtkAccelTranslator.h"
#include "common/AccessibleLabel.h"
#include "i18n/GtkTextCatalog.h"
#include <ao/i18n/MessageCatalog.h>
#include <ao/uimodel/input/KeyChord.h>
#include <ao/uimodel/input/KeymapModel.h>
#include <ao/uimodel/layout/component/LayoutSchema.h>

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
#include <pangomm/layout.h>

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
    using i18n::MessageId;
    using uimodel::ActionCapability;

    constexpr auto kContentMargin = 12;
    constexpr auto kWarningMarginBottom = 6;
    constexpr auto kCategoryMarginTop = 12;

    bool isShortcutEligible(uimodel::ActionSchema const& actionSchema)
    {
      // A global accelerator fires with no widget anchor and no surface to host a popover, so
      // actions that require an anchor or present a menu cannot be driven by one.
      return !actionSchema.supports(ActionCapability::RequiresAnchor) &&
             !actionSchema.supports(ActionCapability::PresentsMenu);
    }

    Gdk::ModifierType accelMods(Gdk::ModifierType state)
    {
      return state & (Gdk::ModifierType::CONTROL_MASK | Gdk::ModifierType::SHIFT_MASK | Gdk::ModifierType::ALT_MASK |
                      Gdk::ModifierType::SUPER_MASK);
    }
  } // namespace

  ShortcutEditorWidget::ShortcutEditorWidget(i18n::MessageCatalog textCatalog,
                                             uimodel::LayoutSchema const& schema,
                                             uimodel::KeymapModel keymap,
                                             ChangedCallback onChanged,
                                             Gtk::Window& hostForDialogs)
    : Gtk::Box{Gtk::Orientation::VERTICAL, 8}
    , _textCatalog{std::move(textCatalog)}
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
        gtkText(_textCatalog, MessageId::GtkShortcutConflictTitle),
        i18n::requiredFormat(
          _textCatalog, MessageId::GtkShortcutConflictMessage, {{"chord", chordText}, {"owner", ownerLabel}}),
        {AppDialogAction{.label = gtkText(_textCatalog, MessageId::GtkCommonCancel),
                         .responseId = Gtk::ResponseType::CANCEL,
                         .role = AppDialogActionRole::Cancel},
         AppDialogAction{.label = gtkText(_textCatalog, MessageId::GtkShortcutReassign),
                         .responseId = Gtk::ResponseType::OK,
                         .role = AppDialogActionRole::Primary}},
        Gtk::ResponseType::OK,
        [respond = std::move(respond)](std::int32_t const responseId)
        { respond(responseId == Gtk::ResponseType::OK); });
    };

    for (auto const& actionSchema : schema.actions())
    {
      if (!isShortcutEligible(actionSchema))
      {
        continue;
      }

      _actions.push_back({.id = actionSchema.id,
                          .label = actionSchema.label.empty() ? actionSchema.id : actionSchema.label,
                          .category = actionSchema.category.empty()
                                        ? gtkText(_textCatalog, MessageId::GtkShortcutOtherCategory)
                                        : actionSchema.category});
      _editableActionIds.push_back(actionSchema.id);
    }

    set_margin(kContentMargin);
    _unmapConn = signal_unmap().connect([this] { closeCapture(); });

    auto* const header = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    auto* const title = Gtk::make_managed<Gtk::Label>(gtkText(_textCatalog, MessageId::GtkShortcutTitle));
    title->set_xalign(0.0F);
    title->set_hexpand(true);
    title->add_css_class("title-4");
    header->append(*title);

    auto* const resetAllButton = Gtk::make_managed<Gtk::Button>(gtkText(_textCatalog, MessageId::GtkShortcutResetAll));
    resetAllButton->set_tooltip_text(gtkText(_textCatalog, MessageId::GtkShortcutResetAllTooltip));
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
    _callbackScope.close();

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
                       _callbackScope.guard(
                         [this, actionId, chord](bool accepted)
                         {
                           if (accepted)
                           {
                             bindChord(actionId, chord);
                           }
                         }));
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
      auto chords = std::string{};

      for (std::size_t i = std::size_t{0}; i < conflicts.size(); ++i)
      {
        chords += (i == 0 ? "" : "; ");
        chords += conflicts[i].chord.toString();
      }

      auto text = i18n::requiredFormat(_textCatalog, MessageId::GtkShortcutConflicts, {{"chords", chords}});
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
    nameLabel->set_ellipsize(Pango::EllipsizeMode::END);
    nameLabel->set_single_line_mode(true);
    nameLabel->set_tooltip_text(action.label);
    row->append(*nameLabel);

    if (auto const chords = _keymap.chordsFor(action.id); chords.empty())
    {
      auto* const unassigned = Gtk::make_managed<Gtk::Label>(gtkText(_textCatalog, MessageId::GtkShortcutUnassigned));
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
        auto const chordText = chord.toString();
        setTooltipAndAccessibleLabel(
          *removeButton, i18n::requiredFormat(_textCatalog, MessageId::GtkShortcutRemove, {{"chord", chordText}}));
        removeButton->signal_clicked().connect([this, id = action.id, chord] { unbindChord(id, chord); });
        chip->append(*removeButton);

        row->append(*chip);
      }
    }

    auto* const addButton = Gtk::make_managed<Gtk::Button>(gtkText(_textCatalog, MessageId::GtkShortcutAdd));
    addButton->set_tooltip_text(gtkText(_textCatalog, MessageId::GtkShortcutAddTooltip));
    addButton->signal_clicked().connect([this, id = action.id] { beginCapture(id); });
    row->append(*addButton);

    auto* const resetButton = Gtk::make_managed<Gtk::Button>(gtkText(_textCatalog, MessageId::GtkShortcutReset));
    resetButton->set_tooltip_text(gtkText(_textCatalog, MessageId::GtkShortcutResetTooltip));
    resetButton->signal_clicked().connect([this, id = action.id] { resetAction(id); });
    row->append(*resetButton);

    return *row;
  }

  void ShortcutEditorWidget::beginCapture(std::string actionId)
  {
    closeCapture();

    auto capturePtr = std::make_unique<Gtk::Window>();
    capturePtr->set_title(gtkText(_textCatalog, MessageId::GtkShortcutCaptureTitle));
    capturePtr->set_transient_for(_hostForDialogs);
    capturePtr->set_modal(true);
    capturePtr->set_resizable(false);

    auto* const box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    box->set_margin(24);

    auto* const prompt = Gtk::make_managed<Gtk::Label>(gtkText(_textCatalog, MessageId::GtkShortcutCapturePrompt));
    box->append(*prompt);

    auto* const hint = Gtk::make_managed<Gtk::Label>(
      i18n::requiredFormat(_textCatalog, MessageId::GtkShortcutCaptureCancelHint, {{"key", "Esc"}}));
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
