// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "app/AppDialog.h"
#include "common/MainContextCallbackScope.h"
#include "list/QueryExpressionBox.h"
#include <ao/CoreIds.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/source/TrackSourceLease.h>

#include <glibmm/refptr.h>
#include <gtkmm/box.h>
#include <gtkmm/columnview.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/entry.h>
#include <gtkmm/label.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/window.h>
#include <sigc++/connection.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Gtk
{
  class Button;
  class ListBoxRow;
  class StringList;
}

namespace ao::rt
{
  class CompletionService;
  class Library;
  class TrackSourceCache;
  class ViewService;
  struct ListNode;
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
                    rt::Library& library,
                    rt::ViewService& views,
                    rt::TrackSourceCache& sources,
                    rt::CompletionService& completion,
                    std::span<rt::CustomTrackPresentationPreset const> customPresets,
                    i18n::MessageCatalog textCatalog,
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

    // Returns the selected presentation ID. Auto is absence of a preference
    // (empty id); a still-selected unavailable option returns its retained
    // opaque id unchanged.
    std::string presentationId() const;

    void configurePlaylistTemplate(std::string_view initialName = {}, std::string_view initialTag = {});
    void setLocalExpression(std::string_view expression);
    void showError(std::string_view message);
    bool tryBeginSubmission();
    void completeSubmission();

    template<typename Callback>
    auto guardPresentationCallback(Callback callback) const
    {
      return _presentationCallbacks.guard(std::move(callback));
    }

  private:
    void buildUi();
    void installUnavailablePresentationOption(std::string_view unavailableId);
    void removeUnavailablePresentationOption();
    std::uint32_t unavailablePresentationRow() const;
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
    Glib::RefPtr<Gtk::StringList> _presentationOptionsPtr;
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
    rt::Library& _library;
    rt::ViewService& _views;
    rt::TrackSourceCache& _sources;
    i18n::MessageCatalog _textCatalog;
    // Snapshot at construction: the presentation option list and the id
    // round-trip both read from here, so the dialog stays internally consistent
    // even if the workspace's custom presets change while it is open.
    std::vector<rt::CustomTrackPresentationPreset> _customPresets;
    ListId _parentListId;
    TrackRowCache const& _trackRowCache;
    std::optional<rt::TrackSourceLease> _optPreviewSourceLease;
    Glib::RefPtr<TrackListModel> _previewModelPtr;

    // Edit mode state
    ListId _editListId{kInvalidListId};
    /// The opaque id of a stored presentation no longer present in the option
    /// snapshot, retained while the dialog's unavailable option exists so a
    /// filter-only or rename edit keeps the user's original preference.
    std::optional<std::string> _optUnavailablePresentationId;
    bool _playlistTemplate = false;
    bool _membershipTagEdited = false;
    bool _syncingMembershipTag = false;
    bool _submissionPending = false;
    MainContextCallbackScope _presentationCallbacks;
  };
} // namespace ao::gtk
