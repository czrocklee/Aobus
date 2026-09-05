// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "common/MainContextCallbackScope.h"
#include <ao/Error.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/uimodel/input/KeyChord.h>
#include <ao/uimodel/input/KeymapModel.h>

#include <gtkmm/box.h>
#include <gtkmm/label.h>
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
  class LayoutSchema;
}

namespace ao::gtk
{
  /**
   * @brief Reusable editor widget for customizing keyboard shortcuts.
   *
   * Lists the shortcut-eligible actions from the @c LayoutSchema and lets the user add,
   * remove, or reset the key chords bound to each. The widget retains the last applied
   * model and any failed candidate separately. Every mutation first asks the changed
   * callback to persist the candidate; live accelerators are published by that callback
   * only after a successful save.
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
    using ChangedCallback = std::function<Result<>(uimodel::KeymapModel const&)>;

    /// Asks the user whether to reassign a chord already bound elsewhere. Invoked with the current
    /// owner's label and the chord text; @p respond must be called with true to proceed with the
    /// reassignment or false to cancel. The default implementation shows a modal AppDialog parented
    /// to the injected host window; tests inject a synchronous stub.
    using ConflictConfirmer = std::function<
      void(std::string const& ownerLabel, std::string const& chordText, std::function<void(bool)> respond)>;

    ShortcutEditorWidget(i18n::MessageCatalog textCatalog,
                         uimodel::LayoutSchema const& schema,
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
    bool hasPendingCandidate() const noexcept { return _optPendingError.has_value(); }
    Result<> retryPending();
    void discardPending();

  private:
    struct EditableAction final
    {
      std::string id;
      std::string label;
      std::string category;
    };

    void rebuild();
    void scheduleRebuild();
    void updateFailureBanner();
    void commit();
    Result<> persistCandidate();
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

    i18n::MessageCatalog _textCatalog;
    Gtk::Window& _hostForDialogs;
    uimodel::KeymapModel _keymap;
    uimodel::KeymapModel _appliedKeymap;
    std::optional<Error> _optPendingError{};
    ChangedCallback _onChanged;
    ConflictConfirmer _conflictConfirmer;
    std::vector<EditableAction> _actions;
    std::vector<std::string> _editableActionIds;

    Gtk::Box* _listBox = nullptr;
    Gtk::Box* _failureBanner = nullptr;
    Gtk::Label* _failureLabel = nullptr;
    std::unique_ptr<Gtk::Window> _captureWindowPtr;
    sigc::connection _captureCloseConn;
    sigc::connection _unmapConn;
    sigc::connection _rebuildConn;
    bool _rebuildQueued = false;
    MainContextCallbackScope _callbackScope;
  };
} // namespace ao::gtk
