// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/input/KeyChord.h>
#include <ao/uimodel/input/KeymapModel.h>

#include <gtkmm/box.h>
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
   * @brief Reusable editor widget for customizing keyboard shortcuts.
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
  class ShortcutEditorWidget final : public Gtk::Box
  {
  public:
    using ChangedCallback = std::function<void(uimodel::KeymapModel const&)>;

    /// Asks the user whether to reassign a chord already bound elsewhere. Invoked with the current
    /// owner's label and the chord text; @p respond must be called with true to proceed with the
    /// reassignment or false to cancel. The default implementation shows a modal AppDialog parented
    /// to the injected host window; tests inject a synchronous stub.
    using ConflictConfirmer = std::function<
      void(std::string const& ownerLabel, std::string const& chordText, std::function<void(bool)> respond)>;

    ShortcutEditorWidget(uimodel::LayoutActionCatalog const& catalog,
                         uimodel::KeymapModel keymap,
                         ChangedCallback onChanged,
                         Gtk::Window& hostForDialogs);
    ~ShortcutEditorWidget() override;

    ShortcutEditorWidget(ShortcutEditorWidget const&) = delete;
    ShortcutEditorWidget& operator=(ShortcutEditorWidget const&) = delete;
    ShortcutEditorWidget(ShortcutEditorWidget&&) = delete;
    ShortcutEditorWidget& operator=(ShortcutEditorWidget&&) = delete;

    /// Replaces the prompt shown when a captured chord collides with another action's binding.
    void setConflictConfirmer(ConflictConfirmer confirmer) { _conflictConfirmer = std::move(confirmer); }

    std::vector<std::string> const& editableActionIds() const { return _editableActionIds; }
    Gtk::Window* captureWindow() const { return _captureWindowPtr.get(); }

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
    bool bindChord(std::string const& actionId, uimodel::KeyChord const& chord);
    void requestBind(std::string const& actionId, uimodel::KeyChord const& chord);
    std::optional<std::string> conflictingOwner(std::string const& actionId, uimodel::KeyChord const& chord) const;
    bool unbindChord(std::string const& actionId, uimodel::KeyChord const& chord);
    void resetAction(std::string const& actionId);
    void resetAll();
    void beginCapture(std::string actionId);
    void closeCapture();
    std::string labelFor(std::string const& actionId) const;

    Gtk::Window& _hostForDialogs;
    uimodel::KeymapModel _keymap;
    ChangedCallback _onChanged;
    ConflictConfirmer _conflictConfirmer;
    std::vector<EditableAction> _actions;
    std::vector<std::string> _editableActionIds;

    Gtk::Box* _listBox = nullptr;
    std::unique_ptr<Gtk::Window> _captureWindowPtr;
    sigc::connection _captureCloseConn;
    sigc::connection _unmapConn;
    std::shared_ptr<bool> _aliveTokenPtr = std::make_shared<bool>(true);
  };
} // namespace ao::gtk
