// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "app/AppDialog.h"
#include "common/MainContextCallbackScope.h"
#include "layout/component/track/TrackCreditsEditor.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/LifetimeScope.h>
#include <ao/async/Subscription.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/TrackField.h>
#include <ao/uimodel/library/property/TrackPropertiesFormModel.h>
#include <ao/uimodel/library/property/TrackPropertiesFormSpec.h>
#include <ao/uimodel/library/track/TrackAuthoringSessions.h>

#include <gtkmm/box.h>
#include <gtkmm/entry.h>
#include <gtkmm/enums.h>
#include <gtkmm/label.h>
#include <gtkmm/notebook.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/widget.h>
#include <sigc++/scoped_connection.h>

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace Gtk
{
  class Button;
  class ToggleButton;
}

namespace ao::rt
{
  class CompletionService;
}

namespace ao::async
{
  class Runtime;
}

namespace ao::uimodel
{
  class LibraryEditing;
}

namespace ao::gtk
{
  class EntryCompletionController;

  class TrackPropertiesDialog final : public AppDialog
  {
  public:
    TrackPropertiesDialog(Gtk::Window& parent,
                          async::Runtime& asyncRuntime,
                          uimodel::LibraryEditing editing,
                          rt::CompletionService& completion,
                          i18n::MessageCatalog textCatalog,
                          std::span<TrackId const> trackIds);
    ~TrackPropertiesDialog() override;

    TrackPropertiesDialog(TrackPropertiesDialog const&) = delete;
    TrackPropertiesDialog& operator=(TrackPropertiesDialog const&) = delete;
    TrackPropertiesDialog(TrackPropertiesDialog&&) = delete;
    TrackPropertiesDialog& operator=(TrackPropertiesDialog&&) = delete;

  private:
    enum class InteractionState : std::uint8_t
    {
      Editing,
      Submitting,
      Closing,
    };

    struct FieldEditor final
    {
      rt::TrackField field = rt::TrackField::Title;
      Gtk::Widget* widget = nullptr;
      uimodel::TrackPropertiesFormEditorKind kind = uimodel::TrackPropertiesFormEditorKind::ReadonlyText;
      Gtk::ToggleButton* clearButton = nullptr;
      bool invalid = false;
    };

    struct CompletionControllerBinding final
    {
      Gtk::Entry* entry = nullptr;
      std::unique_ptr<EntryCompletionController> controllerPtr;
    };

    void buildUi();
    void buildMetadataTab();
    void buildPropertiesTab();
    Result<> prepareEditing(uimodel::LibraryEditing editing, std::span<TrackId const> trackIds);
    void applyLoadedFields();
    void handleSaveClicked();
    void setInteractionState(InteractionState state);
    void updateEditorSensitivity();
    void updateSaveEnabled();
    void bindEditor(FieldEditor& editor);
    void updateEntryValue(FieldEditor& editor, Gtk::Entry& entry);
    void toggleFieldClear(FieldEditor& editor, Gtk::Entry& entry);
    bool hasInvalidInput() const;
    bool hasUnsavedCredits() const noexcept;
    void promptDiscardCredits();

    Gtk::Widget* createEditorWidget(rt::TrackField field, uimodel::TrackPropertiesFormEditorKind editorKind);
    Gtk::Widget* createReadonlyWidget(rt::TrackField field);
    void applyRowView(Gtk::Widget* widget, uimodel::TrackPropertiesFormRowView const& view);
    void setWidgetValue(Gtk::Widget* widget, std::string_view value);

    async::Runtime& _asyncRuntime;
    rt::CompletionService& _completion;
    i18n::MessageCatalog _textCatalog;
    std::optional<uimodel::TrackAuthoringSession> _optEditSession;
    async::Subscription _editSessionInvalidatedSubscription;
    uimodel::TrackPropertiesFormModel _formModel;
    uimodel::TrackPropertiesFormSpec _formSpec;
    Gtk::Button* _saveButton = nullptr;
    InteractionState _interactionState = InteractionState::Editing;
    // Non-owning: the managed prompt closes itself and is destroyed with this dialog.
    AppDialog* _discardPrompt = nullptr;
    bool _discardConfirmed = false;

    Gtk::Box _contentBox{Gtk::Orientation::VERTICAL};
    Gtk::Label _sessionErrorLabel{};
    Gtk::Notebook _notebook{};

    Gtk::ScrolledWindow _metadataScroll{};
    Gtk::Box _metadataBox{Gtk::Orientation::VERTICAL};

    Gtk::ScrolledWindow _propertiesScroll{};
    Gtk::Box _propertiesBox{Gtk::Orientation::VERTICAL};

    layout::TrackCreditsEditor _creditsEditor;
    // End insertion preserves references captured by the scoped editor connections; do not erase or insert in the
    // middle.
    std::deque<FieldEditor> _editors;
    std::vector<FieldEditor> _readonlyRows;
    std::vector<CompletionControllerBinding> _completionControllers;
    std::vector<sigc::scoped_connection> _editorConnections;
    async::LifetimeScope _tasks;
    MainContextCallbackScope _callbackScope;
  };
} // namespace ao::gtk
