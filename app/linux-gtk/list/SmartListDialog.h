// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "app/AppDialog.h"
#include "list/QueryExpressionBox.h"
#include <ao/CoreIds.h>
#include <ao/rt/library/LibraryWriter.h>

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

namespace Gtk
{
  class Button;
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

namespace ao::gtk
{
  class TrackListModel;
  class TrackRowCache;

  class SmartListDialog final : public AppDialog
  {
  public:
    SmartListDialog(Gtk::Window& parent, rt::AppRuntime& runtime, ListId parentListId, TrackRowCache const& provider);
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
    rt::LibraryWriter::ListDraft draft() const;

    // Returns the selected presentation ID. Auto is resolved to a concrete ID.
    std::string presentationId() const;

    void setLocalExpression(std::string_view expression);

  private:
    friend class SmartListDialogTestPeer;

    void setupUi();
    void setupPreview();
    void setupPreviewColumns();
    void rebuildPreviewSource();
    void updateSourceLabels();
    void updateDialogState();
    void updatePreview();

    Gtk::Entry _nameEntry;
    Gtk::Entry _descEntry;
    QueryExpressionBox _exprBox;
    Gtk::DropDown _presentationDropDown;
    Gtk::Button* _okButton = nullptr;
    Gtk::Button* _cancelButton = nullptr;
    Gtk::Box _leftPanel;
    Gtk::Box _rightPanel;
    Gtk::Label _inheritedExprLabel;
    Gtk::Label _effectiveExprLabel;
    Gtk::Label _matchCountLabel;
    Gtk::Label _errorLabel;
    Gtk::ScrolledWindow _previewScrolledWindow;
    Gtk::ColumnView _previewColumnView;
    sigc::connection _exprTimeoutConnection;
    sigc::connection _rebuildConnection;

    // Preview infrastructure
    rt::AppRuntime& _runtime;
    ListId _parentListId;
    TrackRowCache const& _trackRowCache;
    std::unique_ptr<rt::SmartListSource> _previewFilteredListPtr;
    std::unique_ptr<rt::SmartListEvaluator> _previewEnginePtr;
    Glib::RefPtr<TrackListModel> _previewModelPtr;
    bool _expressionValid = true;

    // Edit mode state
    ListId _editListId{kInvalidListId};
  };
} // namespace ao::gtk
