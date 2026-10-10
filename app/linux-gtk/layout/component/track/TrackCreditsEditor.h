// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/async/LifetimeScope.h>
#include <ao/async/Subscription.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/library/Credits.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/projection/TrackDetailSnapshot.h>
#include <ao/uimodel/library/detail/TrackCredits.h>
#include <ao/uimodel/library/track/TrackAuthoringSessions.h>

#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/enums.h>
#include <gtkmm/label.h>
#include <gtkmm/widget.h>
#include <sigc++/scoped_connection.h>

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ao::async
{
  class Runtime;
}
namespace ao::rt
{
  class Library;
  class CompletionService;
  class NotificationService;
}
namespace ao::uimodel
{
  class TrackPropertiesFormModel;
}

namespace ao::gtk::layout
{
  class TrackDetailScope;
  class TrackDetailUndoController;

  // Disposable rows own their completion attachments. Properties borrows its parent
  // form and stages changes only; the detail owner submits through its captured session.
  class TrackCreditsEditor final : public Gtk::Widget
  {
  public:
    TrackCreditsEditor(async::Runtime& asyncRuntime,
                       rt::Library& library,
                       rt::CompletionService& completion,
                       rt::NotificationService& notifications,
                       i18n::MessageCatalog textCatalog,
                       TrackDetailScope* scope,
                       TrackDetailUndoController* undo);
    TrackCreditsEditor(rt::CompletionService& completion,
                       i18n::MessageCatalog textCatalog,
                       uimodel::TrackPropertiesFormModel& form,
                       std::function<void()> changed);
    ~TrackCreditsEditor() override;

    TrackCreditsEditor(TrackCreditsEditor const&) = delete;
    TrackCreditsEditor& operator=(TrackCreditsEditor const&) = delete;
    TrackCreditsEditor(TrackCreditsEditor&&) = delete;
    TrackCreditsEditor& operator=(TrackCreditsEditor&&) = delete;

    void updateVisibility(bool metadataExpanded, bool showEmpty);
    void refresh();

  protected:
    Gtk::SizeRequestMode get_request_mode_vfunc() const override;
    void size_allocate_vfunc(int width, int height, int baseline) override;
    void measure_vfunc(Gtk::Orientation orientation,
                       int forSize,
                       int& minimum,
                       int& natural,
                       int& minimumBaseline,
                       int& naturalBaseline) const override;

  private:
    struct Row;
    struct PreparedEdit;
    enum class FocusTarget : std::uint8_t
    {
      None,
      Draft,
      EntryPoint
    };
    void initialize();
    uimodel::TrackCreditsEditorModel& draft();
    uimodel::TrackCreditSections sections() const;
    void handleSnapshot(rt::TrackDetailSnapshot const& snapshot);
    void beginEditing(std::bitset<library::kCreditKindCount> kinds);
    void cancelEditing();
    void setSession(uimodel::TrackAuthoringSession session);
    void resetSession();
    void clearCredits();
    void commit();
    void submit(rt::MetadataPatch patch, bool clear);
    void queueRender();
    void render();
    void renderCategories(uimodel::TrackCreditSections const& currentSections, bool editing);
    void renderEditingRow(Row& row, std::size_t index, std::size_t count);
    void connectRowSignals(Row& row, std::size_t index);
    void renderDisplayRow(Row& row, uimodel::TrackCreditDisplayRow const& displayRow);
    void retireRowFocus();
    void restoreFocus();
    void refreshValidation();
    Result<PreparedEdit> prepareEditing() const;
    void reportError(std::string const& message);
    library::CreditKind addKind() const;

    // Optional detail-only services; absent in a Properties child.
    async::Runtime* _async = nullptr;
    rt::Library* _library = nullptr;
    rt::CompletionService& _completion;
    rt::NotificationService* _notifications = nullptr;
    i18n::MessageCatalog _textCatalog;
    TrackDetailScope* _scope = nullptr;
    TrackDetailUndoController* _undo = nullptr;
    // Parent form outlives this widget and every disposable row.
    uimodel::TrackPropertiesFormModel* _form = nullptr;
    std::function<void()> _changed;
    rt::TrackDetailSnapshot _snapshot;
    uimodel::TrackCreditSections _baseline;
    uimodel::TrackCreditsEditorModel _draft;
    std::optional<uimodel::TrackAuthoringSession> _optSession;
    async::Subscription _sessionInvalidatedSubscription;
    std::uint64_t _generation = 0;
    bool _metadataExpanded = true;
    bool _showEmpty = false;
    bool _renderQueued = false;
    bool _submitting = false;
    FocusTarget _focusTarget = FocusTarget::None;
    // Points only to stable member buttons, never a disposable row widget.
    Gtk::Button* _entryPoint = nullptr;
    Gtk::Box _contentBox{Gtk::Orientation::VERTICAL, 4};
    Gtk::Label _heading;
    Gtk::Label _scopeLabel;
    std::array<Gtk::Button, library::kCreditKindCount> _categoryButtons;
    std::array<sigc::scoped_connection, library::kCreditKindCount> _categoryConnections;
    Gtk::Box _rowsBox{Gtk::Orientation::VERTICAL, 4};
    Gtk::Box _actions{Gtk::Orientation::VERTICAL, 4};
    Gtk::Button _editButton;
    Gtk::Button _replaceButton;
    Gtk::Button _addButton;
    Gtk::Button _clearButton;
    Gtk::Button _commitButton;
    Gtk::Button _cancelButton;
    Gtk::Label _validation;
    std::vector<std::unique_ptr<Row>> _rows;
    sigc::scoped_connection _scopeConnection;
    sigc::scoped_connection _editConnection;
    sigc::scoped_connection _replaceConnection;
    sigc::scoped_connection _addConnection;
    sigc::scoped_connection _clearConnection;
    sigc::scoped_connection _commitConnection;
    sigc::scoped_connection _cancelConnection;
    async::LifetimeScope _tasks;
  };
} // namespace ao::gtk::layout
