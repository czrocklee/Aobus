// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "app/AppDialog.h"
#include "track/TrackFieldUi.h"
#include <ao/Type.h>
#include <ao/library/FileManifestStore.h>
#include <ao/rt/TrackField.h>

#include <gtkmm/box.h>
#include <gtkmm/entry.h>
#include <gtkmm/enums.h>
#include <gtkmm/notebook.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/widget.h>

#include <memory>
#include <string_view>
#include <vector>

namespace ao::library
{
  class DictionaryStore;
  class MusicLibrary;
  class TrackView;
}

namespace ao::rt
{
  class CompletionService;
  class LibraryMutationService;
}

namespace ao::gtk
{
  class TrackRowCache;
  class EntryCompletionController;

  class TrackPropertiesDialog final : public AppDialog
  {
  public:
    TrackPropertiesDialog(Gtk::Window& parent,
                          library::MusicLibrary& library,
                          rt::LibraryMutationService& mutation,
                          rt::CompletionService& completion,
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
      rt::TrackField field;
      Gtk::Widget* widget = nullptr;
      bool mixed = false;
      TrackFieldRawValue originalRawValue{};
    };

    struct CompletionControllerBinding final
    {
      Gtk::Entry* entry = nullptr;
      std::unique_ptr<EntryCompletionController> controllerPtr;
    };

    void setupUi();
    void setupMetadataTab();
    void setupPropertiesTab();
    void loadData();
    void loadFirstTrack(library::TrackView const& view,
                        library::DictionaryStore const& dictionary,
                        library::FileManifestStore::Reader const* manifestReader = nullptr);
    void loadSubsequentTrack(library::TrackView const& view,
                             library::DictionaryStore const& dictionary,
                             library::FileManifestStore::Reader const* manifestReader = nullptr);
    void onSave();

    Gtk::Widget* createEditorWidget(rt::TrackField field);
    Gtk::Widget* createReadonlyWidget(rt::TrackField field);
    void setWidgetValue(rt::TrackField field, Gtk::Widget* widget, std::string_view value);
    void setEditorMixed(rt::TrackField field, Gtk::Widget* widget);

    library::MusicLibrary& _library;
    rt::LibraryMutationService& _mutation;
    rt::CompletionService& _completion;
    TrackRowCache& _rowCache;
    std::vector<TrackId> _trackIds;
    bool _multipleTracks = false;

    Gtk::Notebook _notebook;

    Gtk::ScrolledWindow _metadataScroll;
    Gtk::Box _metadataBox{Gtk::Orientation::VERTICAL};

    Gtk::ScrolledWindow _propertiesScroll;
    Gtk::Box _propertiesBox{Gtk::Orientation::VERTICAL};

    std::vector<FieldEditor> _editors;
    std::vector<FieldEditor> _readonlyRows;
    std::vector<CompletionControllerBinding> _completionControllers;
  };
} // namespace ao::gtk
