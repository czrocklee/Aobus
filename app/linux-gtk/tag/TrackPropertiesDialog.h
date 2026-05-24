// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "ao/Type.h"
#include "ao/library/FileManifestStore.h"
#include "track/TrackFieldUi.h"
#include <ao/rt/TrackField.h>

#include <gtkmm/box.h>
#include <gtkmm/dialog.h>
#include <gtkmm/enums.h>
#include <gtkmm/notebook.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/widget.h>
#include <gtkmm/window.h>

#include <string_view>
#include <vector>

namespace ao::library
{
  class MusicLibrary;
}

namespace ao::rt
{
  class LibraryMutationService;
}

namespace ao::gtk
{
  class TrackRowCache;

  class TrackPropertiesDialog final : public Gtk::Dialog
  {
  public:
    TrackPropertiesDialog(Gtk::Window& parent,
                          library::MusicLibrary& library,
                          rt::LibraryMutationService& mutation,
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
      detail::TrackFieldRawValue originalRawValue{};
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
  };
} // namespace ao::gtk
