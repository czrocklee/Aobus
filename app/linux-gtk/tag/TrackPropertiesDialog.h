// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "app/AppDialog.h"
#include <ao/CoreIds.h>
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

#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace Gtk
{
  class Button;
}

namespace ao::rt
{
  class CompletionService;
  class Library;
  class LibrarySnapshot;
}

namespace ao::async
{
  class Runtime;
}

namespace ao::gtk
{
  class TrackRowCache;
  class EntryCompletionController;

  class TrackPropertiesDialog final : public AppDialog
  {
  public:
    TrackPropertiesDialog(Gtk::Window& parent,
                          async::Runtime& asyncRuntime,
                          rt::Library& library,
                          rt::CompletionService& completion,
                          i18n::MessageCatalog textCatalog,
                          TrackRowCache& rowCache,
                          std::vector<TrackId> trackIds);
    ~TrackPropertiesDialog() override;

    TrackPropertiesDialog(TrackPropertiesDialog const&) = delete;
    TrackPropertiesDialog& operator=(TrackPropertiesDialog const&) = delete;
    TrackPropertiesDialog(TrackPropertiesDialog&&) = delete;
    TrackPropertiesDialog& operator=(TrackPropertiesDialog&&) = delete;

  private:
    struct FieldEditor final
    {
      rt::TrackField field = rt::TrackField::Title;
      Gtk::Widget* widget = nullptr;
    };

    struct CompletionControllerBinding final
    {
      Gtk::Entry* entry = nullptr;
      std::unique_ptr<EntryCompletionController> controllerPtr;
    };

    void buildUi();
    void buildMetadataTab();
    void buildPropertiesTab();
    void loadSelectedTrackFields();
    void loadFirstTrack(rt::LibrarySnapshot const& scope, TrackId trackId);
    void loadSubsequentTrack(rt::LibrarySnapshot const& scope, TrackId trackId);
    void handleSaveClicked();
    void updateSaveEnabled();
    void updateEditorValue(rt::TrackField field, Gtk::Widget* widget);

    Gtk::Widget* createEditorWidget(rt::TrackField field, uimodel::TrackPropertiesFormEditorKind editorKind);
    Gtk::Widget* createReadonlyWidget(rt::TrackField field);
    void applyRowView(Gtk::Widget* widget, uimodel::TrackPropertiesFormRowView const& view);
    void setWidgetValue(Gtk::Widget* widget, std::string_view value);
    void setEditorMixed(Gtk::Widget* widget, std::string_view text);

    rt::Library& _library;
    async::Runtime& _asyncRuntime;
    rt::CompletionService& _completion;
    i18n::MessageCatalog _textCatalog;
    TrackRowCache& _rowCache;
    std::vector<TrackId> _trackIds;
    std::optional<uimodel::TrackAuthoringSession> _optEditSession;
    async::Subscription _editSessionInvalidatedSubscription;
    bool _multipleTracks = false;
    uimodel::TrackPropertiesFormModel _formModel;
    Gtk::Button* _saveButton = nullptr;

    Gtk::Box _contentBox{Gtk::Orientation::VERTICAL};
    Gtk::Label _sessionErrorLabel;
    Gtk::Notebook _notebook;

    Gtk::ScrolledWindow _metadataScroll;
    Gtk::Box _metadataBox{Gtk::Orientation::VERTICAL};

    Gtk::ScrolledWindow _propertiesScroll;
    Gtk::Box _propertiesBox{Gtk::Orientation::VERTICAL};

    std::vector<FieldEditor> _editors;
    std::vector<FieldEditor> _readonlyRows;
    std::vector<CompletionControllerBinding> _completionControllers;
    async::LifetimeScope _tasks;
  };
} // namespace ao::gtk
