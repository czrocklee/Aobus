// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/input/KeyChord.h>
#include <ao/uimodel/input/KeymapModel.h>

#include <gtkmm/box.h>
#include <gtkmm/enums.h>
#include <gtkmm/widget.h>
#include <gtkmm/window.h>
#include <sigc++/connection.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ao::uimodel
{
  class LayoutActionCatalog;
}

namespace ao::gtk
{
  /**
   * @brief Standalone editor window for customizing keyboard shortcuts.
   *
   * Lists the shortcut-eligible actions from the @c LayoutActionCatalog and lets the user add,
   * remove, or reset the key chords bound to each. The model held here is the single source
   * of truth for the edit session; every mutation re-renders the list and invokes the
   * supplied changed-callback so the caller can persist the delta and re-apply accelerators.
   *
   * Conflicts are surfaced explicitly: capturing a chord already used by another action prompts
   * the user to confirm the reassignment before it is taken, so the effective keymap stays
   * unambiguous and no binding is removed silently.
   *
   * This is the GTK *view*; all reusable keymap logic lives in @c uimodel::KeymapModel,
   * and GDK keysym translation is confined to @c GtkAccelTranslator.
   */
  class KeyboardShortcutsWindow final : public Gtk::Window
  {
  public:
    using ChangedCallback = std::function<void(uimodel::KeymapModel const&)>;

    /// Asks the user whether to reassign a chord already bound elsewhere. Invoked with the current
    /// owner's label and the chord text; @p respond must be called with true to proceed with the
    /// reassignment or false to cancel. The default implementation shows a modal Gtk::AlertDialog;
    /// tests inject a synchronous stub.
    using ConflictConfirmer = std::function<
      void(std::string const& ownerLabel, std::string const& chordText, std::function<void(bool)> respond)>;

    KeyboardShortcutsWindow(uimodel::LayoutActionCatalog const& catalog,
                            uimodel::KeymapModel keymap,
                            ChangedCallback onChanged);
    ~KeyboardShortcutsWindow() override;

    KeyboardShortcutsWindow(KeyboardShortcutsWindow const&) = delete;
    KeyboardShortcutsWindow& operator=(KeyboardShortcutsWindow const&) = delete;
    KeyboardShortcutsWindow(KeyboardShortcutsWindow&&) = delete;
    KeyboardShortcutsWindow& operator=(KeyboardShortcutsWindow&&) = delete;

    // Editing operations driven by the in-window controls. They are public so the behavior is
    // unit-testable without synthesizing live GDK key events through the capture popup.

    /// Binds @p chord to @p actionId, transferring it away from any previous owner. This is the
    /// model-level primitive (the user has already confirmed any reassignment); the interactive
    /// capture flow goes through requestBind() instead. Returns true when the effective keymap
    /// changed.
    bool bindChord(std::string const& actionId, uimodel::KeyChord const& chord);

    /// Binds @p chord to @p actionId, but when the chord is already held by a different action the
    /// reassignment is confirmed with the user first (see ConflictConfirmer) rather than taken
    /// silently. This is what the capture popup invokes.
    void requestBind(std::string const& actionId, uimodel::KeyChord const& chord);

    /// The action currently bound to @p chord other than @p actionId, if any. Drives the
    /// reassignment prompt.
    std::optional<std::string> conflictingOwner(std::string const& actionId, uimodel::KeyChord const& chord) const;

    /// Replaces the prompt shown when a captured chord collides with another action's binding.
    void setConflictConfirmer(ConflictConfirmer confirmer) { _conflictConfirmer = std::move(confirmer); }
    /// Removes @p chord from @p actionId. Returns true when it was bound.
    bool unbindChord(std::string const& actionId, uimodel::KeyChord const& chord);
    void resetAction(std::string const& actionId);
    void resetAll();

    uimodel::KeymapModel const& keymap() const { return _keymap; }
    std::vector<std::string> const& editableActionIds() const { return _editableActionIds; }

  private:
    struct EditableAction final
    {
      std::string id;
      std::string label;
      std::string category;
    };

    void rebuild();
    void commit();
    Gtk::Widget& buildActionRow(EditableAction const& action);
    void beginCapture(std::string actionId);
    void closeCapture();
    std::string labelFor(std::string const& actionId) const;

    uimodel::KeymapModel _keymap;
    ChangedCallback _onChanged;
    ConflictConfirmer _conflictConfirmer;
    std::vector<EditableAction> _actions;
    std::vector<std::string> _editableActionIds;

    Gtk::Box _content{Gtk::Orientation::VERTICAL, 8};
    Gtk::Box* _listBox = nullptr;
    std::unique_ptr<Gtk::Window> _captureWindowPtr;
    sigc::connection _captureCloseConn;
  };
} // namespace ao::gtk
