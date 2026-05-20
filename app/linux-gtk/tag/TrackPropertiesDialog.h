// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "ao/Type.h"

#include <gtkmm/box.h>
#include <gtkmm/dialog.h>
#include <gtkmm/entry.h>
#include <gtkmm/enums.h>
#include <gtkmm/label.h>
#include <gtkmm/notebook.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/spinbutton.h>
#include <gtkmm/window.h>

#include <cstdint>
#include <string>
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
    struct AggregatedFields final
    {
      std::string title{};
      std::string artist{};
      std::string album{};
      std::string albumArtist{};
      std::string genre{};
      std::string composer{};
      std::string work{};
      std::uint16_t year = 0;
      std::uint16_t trackNumber = 0;
      std::uint16_t totalTracks = 0;
      std::uint16_t discNumber = 0;
      std::uint16_t totalDiscs = 0;
      bool titleMixed = false;
      bool artistMixed = false;
      bool albumMixed = false;
      bool albumArtistMixed = false;
      bool genreMixed = false;
      bool composerMixed = false;
      bool workMixed = false;
      bool yearMixed = false;
      bool trackNumberMixed = false;
      bool totalTracksMixed = false;
      bool discNumberMixed = false;
      bool totalDiscsMixed = false;
    };

    void setupUi();
    void setupMetadataTab();
    void setupPropertiesTab();
    void loadData();
    void onSave();
    void applyMixedPlaceholder(Gtk::Entry& entry, bool mixed);

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

    Gtk::Entry _titleEntry;
    Gtk::Entry _artistEntry;
    Gtk::Entry _albumArtistEntry;
    Gtk::Entry _albumEntry;
    Gtk::Entry _genreEntry;
    Gtk::Entry _composerEntry;
    Gtk::Entry _workEntry;
    Gtk::SpinButton _yearSpin;
    Gtk::SpinButton _trackNumberSpin;
    Gtk::SpinButton _totalTracksSpin;
    Gtk::SpinButton _discNumberSpin;
    Gtk::SpinButton _totalDiscsSpin;

    Gtk::Label _filePathLabel;
    Gtk::Label _codecLabel;
    Gtk::Label _sampleRateLabel;
    Gtk::Label _channelsLabel;
    Gtk::Label _bitDepthLabel;
    Gtk::Label _bitrateLabel;
    Gtk::Label _durationLabel;
    Gtk::Label _fileSizeLabel;
    Gtk::Label _modifiedLabel;

    AggregatedFields _fields{};
  };
} // namespace ao::gtk
