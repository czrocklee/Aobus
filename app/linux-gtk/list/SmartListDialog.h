// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "app/AppDialog.h"
#include "common/MainContextCallbackScope.h"
#include "i18n/GtkTextCatalog.h"
#include "list/QueryExpressionBox.h"
#include <ao/CoreIds.h>
#include <ao/rt/ListMutation.h>
#include <ao/uimodel/presentation/PresentationTextCatalog.h>

#include <gtkmm/box.h>
#include <gtkmm/columnview.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/entry.h>
#include <gtkmm/label.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/window.h>
#include <sigc++/connection.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace Gtk
{
  class Button;
  class ListBoxRow;
}

namespace ao::rt
{
  class AppRuntime;
  class AllTracksSource;
  struct ListNode;
  class SmartListSource;
  class SmartListEvaluator;
  class TrackSource;
}

namespace ao::uimodel
{
  struct SmartListEditorViewState;
}

namespace ao::gtk
{
  class TrackListModel;
  class TrackRowCache;

  class SmartListDialog final : public AppDialog
  {
  public:
    SmartListDialog(Gtk::Window& parent,
                    rt::AppRuntime& runtime,
                    uimodel::PresentationTextCatalog const& textCatalog,
                    GtkTextCatalog gtkTextCatalog,
                    ListId parentListId,
                    TrackRowCache const& provider);
    ~SmartListDialog() override;

    SmartListDialog(SmartListDialog const&) = delete;
    SmartListDialog& operator=(SmartListDialog const&) = delete;
    SmartListDialog(SmartListDialog&&) = delete;
    SmartListDialog& operator=(SmartListDialog&&) = delete;

    // Populate dialog fields from an existing list for editing
    void populate(ListId id, rt::ListNode const& node, std::optional<std::string> const& optPresentationId);

    // Returns the ListId for update (0 if creating a new list)
    ListId editListId() const;

    // Returns a ListDraft populated from the dialog fields
    rt::ListDraft draft() const;

    // Returns the selected presentation ID. Auto is resolved to a concrete ID.
    std::string presentationId() const;

    void configurePlaylistTemplate(std::string_view initialName = {}, std::string_view initialTag = {});
    void setLocalExpression(std::string_view expression);
    void showError(std::string_view message);
    bool beginSubmission();
    void completeSubmission();

    template<typename Callback>
    auto guardPresentationCallback(Callback callback) const
    {
      return _presentationCallbacks.guard(std::move(callback));
    }

  private:
    void buildUi();
    void buildPreview();
    void configurePreviewColumns();
    void rebuildPreviewSource();
    void updateSourceLabels();
    void updatePlaylistExpression();
    void updatePlaylistTagFromName();
    uimodel::SmartListEditorViewState editorViewState() const;
    void updateDialogState();
    void updatePreview();

    Gtk::Entry _nameEntry;
    Gtk::Entry _descEntry;
    Gtk::Entry _membershipTagEntry;
    QueryExpressionBox _exprBox;
    Gtk::DropDown _presentationDropDown;
    Gtk::Button* _okButton = nullptr;
    Gtk::Button* _cancelButton = nullptr;
    Gtk::Box _leftPanel;
    Gtk::Box _rightPanel;
    Gtk::Label _inheritedExprLabel;
    Gtk::Label _effectiveExprLabel;
    Gtk::Label _membershipEditingLabel;
    Gtk::Label _matchCountLabel;
    Gtk::Label _errorLabel;
    Gtk::ListBoxRow* _membershipTagRow = nullptr;
    Gtk::ScrolledWindow _previewScrolledWindow;
    Gtk::ColumnView _previewColumnView;
    sigc::connection _exprTimeoutConnection;
    sigc::connection _rebuildConnection;

    // Preview infrastructure
    rt::AppRuntime& _runtime;
    uimodel::PresentationTextCatalog _textCatalog;
    GtkTextCatalog _gtkTextCatalog;
    ListId _parentListId;
    TrackRowCache const& _trackRowCache;
    std::shared_ptr<rt::SmartListSource> _previewFilteredListPtr;
    std::unique_ptr<rt::SmartListEvaluator> _previewEnginePtr;
    Glib::RefPtr<TrackListModel> _previewModelPtr;

    // Edit mode state
    ListId _editListId{kInvalidListId};
    bool _playlistTemplate = false;
    bool _membershipTagEdited = false;
    bool _syncingMembershipTag = false;
    bool _submissionPending = false;
    MainContextCallbackScope _presentationCallbacks;
  };
} // namespace ao::gtk
