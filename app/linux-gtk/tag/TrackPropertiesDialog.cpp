// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tag/TrackPropertiesDialog.h"

#include "ao/Type.h"
#include "ao/library/DictionaryStore.h"
#include "ao/library/MusicLibrary.h"
#include "ao/library/TrackStore.h"
#include "ao/library/TrackView.h"
#include "layout/LayoutConstants.h"
#include "runtime/LibraryMutationService.h"
#include "runtime/StateTypes.h"
#include "track/TrackRowCache.h"

#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/dialog.h>
#include <gtkmm/entry.h>
#include <gtkmm/enums.h>
#include <gtkmm/label.h>
#include <gtkmm/notebook.h>
#include <gtkmm/object.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/spinbutton.h>
#include <gtkmm/widget.h>
#include <gtkmm/window.h>
#include <pangomm/layout.h>

#include <chrono>
#include <cstdint>
#include <ctime>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::gtk
{
  namespace
  {
    constexpr int kDialogDefaultWidth = 520;
    constexpr int kDialogDefaultHeight = 580;
    constexpr int kBoxSpacing = layout::kSpacingMedium;
    constexpr int kSectionSpacing = layout::kSpacingLarge;
    constexpr int kFieldRowSpacing = layout::kSpacingSmall;
    constexpr float kLabelOpacity = 0.6F;
    constexpr int kLabelWidthChars = 14;
    constexpr std::uint16_t kCodecIdFlac = 3;
    constexpr std::uint16_t kCodecIdMp3 = 0x55;
    constexpr double kKhzMultiplier = 1000.0;
    constexpr int kSecondsPerHour = 3600;
    constexpr int kSecondsPerMinute = 60;
    constexpr int kYearMin = 0;
    constexpr int kYearMax = 9999;
    constexpr int kTrackNumberMin = 0;
    constexpr int kTrackNumberMax = 999;
    constexpr int kSpinButtonPageIncrement = 10;
    constexpr int kSpinButtonStepIncrement = 1;
    constexpr int kMixedSpinValue = -1;

    Glib::ustring formatCodec(std::uint16_t codecId)
    {
      switch (codecId)
      {
        case kCodecIdFlac: return "FLAC";
        case kCodecIdMp3: return "MP3";
        default: return codecId == 0 ? "Unknown" : std::format("Codec (0x{:02x})", codecId);
      }
    }

    std::string formatSampleRate(std::uint32_t rate)
    {
      if (rate == 0)
      {
        return "Unknown";
      }

      if (rate % 1000 == 0)
      {
        return std::format("{} kHz", rate / 1000);
      }

      return std::format("{:.1f} kHz", static_cast<double>(rate) / kKhzMultiplier);
    }

    std::string formatDuration(std::uint32_t durationMs)
    {
      if (durationMs == 0)
      {
        return "Unknown";
      }

      auto const totalSeconds = durationMs / 1000;
      auto const hours = totalSeconds / kSecondsPerHour;
      auto const minutes = (totalSeconds % kSecondsPerHour) / kSecondsPerMinute;
      auto const seconds = totalSeconds % kSecondsPerMinute;

      if (hours > 0)
      {
        return std::format("{}:{:02}:{:02}", hours, minutes, seconds);
      }

      return std::format("{}:{:02}", minutes, seconds);
    }

    std::string formatFileSize(std::uint64_t bytes)
    {
      if (bytes == 0)
      {
        return "Unknown";
      }

      auto constexpr kKB = 1024ULL;
      auto constexpr kMB = kKB * 1024;
      auto constexpr kGB = kMB * 1024;

      if (bytes >= kGB)
      {
        return std::format("{:.2f} GB", static_cast<double>(bytes) / static_cast<double>(kGB));
      }

      if (bytes >= kMB)
      {
        return std::format("{:.2f} MB", static_cast<double>(bytes) / static_cast<double>(kMB));
      }

      return std::format("{:.2f} KB", static_cast<double>(bytes) / static_cast<double>(kKB));
    }

    std::string formatBitrate(std::uint32_t bitrate)
    {
      if (bitrate == 0)
      {
        return "Unknown";
      }

      return std::format("{} kbps", bitrate / 1000);
    }

    std::string formatMtime(std::uint64_t mtime)
    {
      if (mtime == 0)
      {
        return "Unknown";
      }

      auto const timePoint = std::chrono::system_clock::from_time_t(static_cast<std::time_t>(mtime));
      return std::format("{:%Y-%m-%d %H:%M}", timePoint);
    }

    std::string formatChannels(std::uint8_t channels)
    {
      if (channels == 0)
      {
        return "Unknown";
      }

      if (channels == 1)
      {
        return "Mono";
      }

      if (channels == 2)
      {
        return "Stereo";
      }

      return std::format("{} Ch", channels);
    }

    std::string formatBitDepth(std::uint8_t bitDepth)
    {
      if (bitDepth == 0)
      {
        return "Unknown";
      }

      return std::format("{} bit", bitDepth);
    }
  }

  TrackPropertiesDialog::TrackPropertiesDialog(Gtk::Window& parent,
                                               library::MusicLibrary& library,
                                               rt::LibraryMutationService& mutation,
                                               TrackRowCache& rowCache,
                                               std::vector<TrackId> trackIds)
    : Gtk::Dialog{}
    , _library{library}
    , _mutation{mutation}
    , _rowCache{rowCache}
    , _trackIds{std::move(trackIds)}
    , _multipleTracks{_trackIds.size() > 1}
  {
    auto const title =
      _multipleTracks ? std::format("Properties — {} tracks selected", _trackIds.size()) : std::string{"Properties"};

    set_title(title);
    set_transient_for(parent);
    set_modal(true);
    set_default_size(kDialogDefaultWidth, kDialogDefaultHeight);

    setupUi();
    loadData();
  }

  TrackPropertiesDialog::~TrackPropertiesDialog() = default;

  void TrackPropertiesDialog::setupUi()
  {
    auto* const contentArea = get_content_area();
    contentArea->add_css_class("ao-dialog-content");

    _notebook.add_css_class("ao-properties-notebook");
    setupMetadataTab();
    setupPropertiesTab();

    contentArea->append(_notebook);

    auto* const saveButton = Gtk::make_managed<Gtk::Button>("Save");
    saveButton->add_css_class("suggested-action");
    saveButton->signal_clicked().connect([this] { onSave(); });
    add_action_widget(*saveButton, Gtk::ResponseType::OK);

    auto* const closeButton = Gtk::make_managed<Gtk::Button>("Close");
    add_action_widget(*closeButton, Gtk::ResponseType::CLOSE);
  }

  void TrackPropertiesDialog::setupMetadataTab()
  {
    _metadataScroll.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    _metadataScroll.set_vexpand(true);
    _metadataScroll.set_child(_metadataBox);

    _metadataBox.set_spacing(kBoxSpacing);
    _metadataBox.set_valign(Gtk::Align::START);
    _metadataBox.set_margin_start(kSectionSpacing);
    _metadataBox.set_margin_end(kSectionSpacing);
    _metadataBox.set_margin_top(kSectionSpacing);
    _metadataBox.set_margin_bottom(kSectionSpacing);

    auto const createFieldRow = [this](std::string const& labelText, Gtk::Widget& valueWidget)
    {
      auto* const row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, kFieldRowSpacing);
      auto* const label = Gtk::make_managed<Gtk::Label>(labelText);

      label->set_halign(Gtk::Align::START);
      label->set_valign(Gtk::Align::CENTER);
      label->set_width_chars(kLabelWidthChars);
      label->set_xalign(0.0F);
      label->set_opacity(kLabelOpacity);
      label->add_css_class("ao-property-label");

      valueWidget.set_halign(Gtk::Align::FILL);
      valueWidget.set_hexpand(true);
      valueWidget.set_valign(Gtk::Align::CENTER);

      row->append(*label);
      row->append(valueWidget);
      _metadataBox.append(*row);
    };

    _titleEntry.set_placeholder_text("Track Title");
    _titleEntry.set_hexpand(true);
    _artistEntry.set_placeholder_text("Artist");
    _artistEntry.set_hexpand(true);
    _albumArtistEntry.set_placeholder_text("Album Artist");
    _albumArtistEntry.set_hexpand(true);
    _albumEntry.set_placeholder_text("Album");
    _albumEntry.set_hexpand(true);
    _genreEntry.set_placeholder_text("Genre");
    _genreEntry.set_hexpand(true);
    _composerEntry.set_placeholder_text("Composer");
    _composerEntry.set_hexpand(true);
    _workEntry.set_placeholder_text("Work");
    _workEntry.set_hexpand(true);

    _yearSpin.set_range(kYearMin, kYearMax);
    _yearSpin.set_increments(kSpinButtonStepIncrement, kSpinButtonPageIncrement);
    _yearSpin.set_numeric(true);
    _yearSpin.set_hexpand(true);

    _trackNumberSpin.set_range(kTrackNumberMin, kTrackNumberMax);
    _trackNumberSpin.set_increments(kSpinButtonStepIncrement, kSpinButtonPageIncrement);
    _trackNumberSpin.set_numeric(true);
    _trackNumberSpin.set_hexpand(true);

    _totalTracksSpin.set_range(kTrackNumberMin, kTrackNumberMax);
    _totalTracksSpin.set_increments(kSpinButtonStepIncrement, kSpinButtonPageIncrement);
    _totalTracksSpin.set_numeric(true);
    _totalTracksSpin.set_hexpand(true);

    _discNumberSpin.set_range(kTrackNumberMin, kTrackNumberMax);
    _discNumberSpin.set_increments(kSpinButtonStepIncrement, kSpinButtonPageIncrement);
    _discNumberSpin.set_numeric(true);
    _discNumberSpin.set_hexpand(true);

    _totalDiscsSpin.set_range(kTrackNumberMin, kTrackNumberMax);
    _totalDiscsSpin.set_increments(kSpinButtonStepIncrement, kSpinButtonPageIncrement);
    _totalDiscsSpin.set_numeric(true);
    _totalDiscsSpin.set_hexpand(true);

    createFieldRow("Track Title", _titleEntry);
    createFieldRow("Artist", _artistEntry);
    createFieldRow("Album Artist", _albumArtistEntry);
    createFieldRow("Album", _albumEntry);
    createFieldRow("Genre", _genreEntry);
    createFieldRow("Composer", _composerEntry);
    createFieldRow("Work", _workEntry);
    createFieldRow("Year", _yearSpin);
    createFieldRow("Track Number", _trackNumberSpin);
    createFieldRow("Total Tracks", _totalTracksSpin);
    createFieldRow("Disc Number", _discNumberSpin);
    createFieldRow("Total Discs", _totalDiscsSpin);

    _notebook.append_page(_metadataScroll, "Metadata");
  }

  void TrackPropertiesDialog::setupPropertiesTab()
  {
    _propertiesScroll.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    _propertiesScroll.set_vexpand(true);
    _propertiesScroll.set_child(_propertiesBox);

    _propertiesBox.set_spacing(kBoxSpacing);
    _propertiesBox.set_valign(Gtk::Align::START);
    _propertiesBox.set_margin_start(kSectionSpacing);
    _propertiesBox.set_margin_end(kSectionSpacing);
    _propertiesBox.set_margin_top(kSectionSpacing);
    _propertiesBox.set_margin_bottom(kSectionSpacing);

    auto const createPropertyRow = [this](std::string const& labelText, Gtk::Label& valueLabel)
    {
      auto* const row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, kFieldRowSpacing);
      auto* const label = Gtk::make_managed<Gtk::Label>(labelText);

      label->set_halign(Gtk::Align::START);
      label->set_valign(Gtk::Align::CENTER);
      label->set_width_chars(kLabelWidthChars);
      label->set_xalign(0.0F);
      label->set_opacity(kLabelOpacity);
      label->add_css_class("ao-property-label");

      valueLabel.set_halign(Gtk::Align::START);
      valueLabel.set_hexpand(true);
      valueLabel.set_ellipsize(Pango::EllipsizeMode::MIDDLE);
      valueLabel.add_css_class("ao-property-value");

      row->append(*label);
      row->append(valueLabel);
      _propertiesBox.append(*row);
    };

    _filePathLabel.set_selectable(true);
    _filePathLabel.set_ellipsize(Pango::EllipsizeMode::MIDDLE);

    createPropertyRow("File Path", _filePathLabel);
    createPropertyRow("Codec", _codecLabel);
    createPropertyRow("Sample Rate", _sampleRateLabel);
    createPropertyRow("Channels", _channelsLabel);
    createPropertyRow("Bit Depth", _bitDepthLabel);
    createPropertyRow("Bitrate", _bitrateLabel);
    createPropertyRow("Duration", _durationLabel);
    createPropertyRow("File Size", _fileSizeLabel);
    createPropertyRow("Modified", _modifiedLabel);

    _notebook.append_page(_propertiesScroll, "Properties");
  }

  // NOLINTNEXTLINE(readability-function-cognitive-complexity)
  void TrackPropertiesDialog::loadData()
  {
    if (_trackIds.empty())
    {
      return;
    }

    auto const txn = _library.readTransaction();
    auto const reader = _library.tracks().reader(txn);
    auto const& dictionary = _library.dictionary();

    _fields = AggregatedFields{};
    bool first = true;

    auto aggregateTitle = std::string{};
    auto aggregateArtist = std::string{};
    auto aggregateAlbum = std::string{};
    auto aggregateAlbumArtist = std::string{};
    auto aggregateGenre = std::string{};
    auto aggregateComposer = std::string{};
    auto aggregateWork = std::string{};
    std::uint16_t aggregateYear = 0;
    std::uint16_t aggregateTrackNumber = 0;
    std::uint16_t aggregateTotalTracks = 0;
    std::uint16_t aggregateDiscNumber = 0;
    std::uint16_t aggregateTotalDiscs = 0;

    auto aggregateFilePath = std::string{};
    std::uint16_t aggregateCodecId = 0;
    std::uint32_t aggregateSampleRate = 0;
    std::uint8_t aggregateChannels = 0;
    std::uint8_t aggregateBitDepth = 0;
    std::uint32_t aggregateBitrate = 0;
    std::uint32_t aggregateDurationMs = 0;
    std::uint64_t aggregateFileSize = 0;
    std::uint64_t aggregateMtime = 0;
    bool codecMixed = false;
    bool sampleRateMixed = false;
    bool channelsMixed = false;
    bool bitDepthMixed = false;
    bool bitrateMixed = false;
    bool durationMixed = false;
    bool fileSizeMixed = false;
    bool mtimeMixed = false;
    bool uriMixed = false;

    for (auto const trackId : _trackIds)
    {
      auto const optView = reader.get(trackId, library::TrackStore::Reader::LoadMode::Both);

      if (!optView)
      {
        continue;
      }

      auto const& view = *optView; // NOLINT(aobus-readability-use-if-init-statement)

      if (first)
      {
        aggregateTitle = std::string{view.metadata().title()};
        aggregateArtist = std::string{dictionary.getOrDefault(view.metadata().artistId(), "")};
        aggregateAlbum = std::string{dictionary.getOrDefault(view.metadata().albumId(), "")};
        aggregateAlbumArtist = std::string{dictionary.getOrDefault(view.metadata().albumArtistId(), "")};
        aggregateGenre = std::string{dictionary.getOrDefault(view.metadata().genreId(), "")};
        aggregateComposer = std::string{dictionary.getOrDefault(view.metadata().composerId(), "")};
        aggregateWork = std::string{dictionary.getOrDefault(view.metadata().workId(), "")};
        aggregateYear = view.metadata().year();
        aggregateTrackNumber = view.metadata().trackNumber();
        aggregateTotalTracks = view.metadata().totalTracks();
        aggregateDiscNumber = view.metadata().discNumber();
        aggregateTotalDiscs = view.metadata().totalDiscs();

        aggregateFilePath = std::string{view.property().uri()};
        aggregateCodecId = view.property().codecId();
        aggregateSampleRate = view.property().sampleRate();
        aggregateChannels = view.property().channels();
        aggregateBitDepth = view.property().bitDepth();
        aggregateBitrate = view.property().bitrate();
        aggregateDurationMs = view.property().durationMs();
        aggregateFileSize = view.property().fileSize();
        aggregateMtime = view.property().mtime();
        first = false;
      }
      else
      {
        // NOLINTBEGIN(aobus-readability-control-block-spacing)
        if (std::string{view.metadata().title()} != aggregateTitle)
        {
          _fields.titleMixed = true;
        }
        if (std::string{dictionary.getOrDefault(view.metadata().artistId(), "")} != aggregateArtist)
        {
          _fields.artistMixed = true;
        }
        if (std::string{dictionary.getOrDefault(view.metadata().albumId(), "")} != aggregateAlbum)
        {
          _fields.albumMixed = true;
        }
        if (std::string{dictionary.getOrDefault(view.metadata().albumArtistId(), "")} != aggregateAlbumArtist)
        {
          _fields.albumArtistMixed = true;
        }
        if (std::string{dictionary.getOrDefault(view.metadata().genreId(), "")} != aggregateGenre)
        {
          _fields.genreMixed = true;
        }
        if (std::string{dictionary.getOrDefault(view.metadata().composerId(), "")} != aggregateComposer)
        {
          _fields.composerMixed = true;
        }
        if (std::string{dictionary.getOrDefault(view.metadata().workId(), "")} != aggregateWork)
        {
          _fields.workMixed = true;
        }
        if (view.metadata().year() != aggregateYear)
        {
          _fields.yearMixed = true;
        }
        if (view.metadata().trackNumber() != aggregateTrackNumber)
        {
          _fields.trackNumberMixed = true;
        }
        if (view.metadata().totalTracks() != aggregateTotalTracks)
        {
          _fields.totalTracksMixed = true;
        }
        if (view.metadata().discNumber() != aggregateDiscNumber)
        {
          _fields.discNumberMixed = true;
        }
        if (view.metadata().totalDiscs() != aggregateTotalDiscs)
        {
          _fields.totalDiscsMixed = true;
        }

        if (view.property().codecId() != aggregateCodecId)
        {
          codecMixed = true;
        }
        if (view.property().sampleRate() != aggregateSampleRate)
        {
          sampleRateMixed = true;
        }
        if (view.property().channels() != aggregateChannels)
        {
          channelsMixed = true;
        }
        if (view.property().bitDepth() != aggregateBitDepth)
        {
          bitDepthMixed = true;
        }
        if (view.property().bitrate() != aggregateBitrate)
        {
          bitrateMixed = true;
        }
        if (view.property().durationMs() != aggregateDurationMs)
        {
          durationMixed = true;
        }
        if (view.property().fileSize() != aggregateFileSize)
        {
          fileSizeMixed = true;
        }
        if (view.property().mtime() != aggregateMtime)
        {
          mtimeMixed = true;
        }
        if (std::string{view.property().uri()} != aggregateFilePath)
        {
          uriMixed = true;
        }
        // NOLINTEND(aobus-readability-control-block-spacing)
      }
    }

    _fields.title = std::move(aggregateTitle);
    _fields.artist = std::move(aggregateArtist);
    _fields.album = std::move(aggregateAlbum);
    _fields.albumArtist = std::move(aggregateAlbumArtist);
    _fields.genre = std::move(aggregateGenre);
    _fields.composer = std::move(aggregateComposer);
    _fields.work = std::move(aggregateWork);
    _fields.year = aggregateYear;
    _fields.trackNumber = aggregateTrackNumber;
    _fields.totalTracks = aggregateTotalTracks;
    _fields.discNumber = aggregateDiscNumber;
    _fields.totalDiscs = aggregateTotalDiscs;

    applyMixedPlaceholder(_titleEntry, _fields.titleMixed);
    applyMixedPlaceholder(_artistEntry, _fields.artistMixed);
    applyMixedPlaceholder(_albumArtistEntry, _fields.albumArtistMixed);
    applyMixedPlaceholder(_albumEntry, _fields.albumMixed);
    applyMixedPlaceholder(_genreEntry, _fields.genreMixed);
    applyMixedPlaceholder(_composerEntry, _fields.composerMixed);
    applyMixedPlaceholder(_workEntry, _fields.workMixed);

    // NOLINTBEGIN(aobus-readability-control-block-spacing)
    if (!_fields.titleMixed)
    {
      _titleEntry.set_text(_fields.title);
    }
    if (!_fields.artistMixed)
    {
      _artistEntry.set_text(_fields.artist);
    }
    if (!_fields.albumArtistMixed)
    {
      _albumArtistEntry.set_text(_fields.albumArtist);
    }
    if (!_fields.albumMixed)
    {
      _albumEntry.set_text(_fields.album);
    }
    if (!_fields.genreMixed)
    {
      _genreEntry.set_text(_fields.genre);
    }
    if (!_fields.composerMixed)
    {
      _composerEntry.set_text(_fields.composer);
    }
    if (!_fields.workMixed)
    {
      _workEntry.set_text(_fields.work);
    }
    // NOLINTEND(aobus-readability-control-block-spacing)
    if (!_fields.yearMixed)
    {
      _yearSpin.set_value(static_cast<double>(_fields.year));
    }
    else
    {
      _yearSpin.set_value(static_cast<double>(kMixedSpinValue));
      _yearSpin.set_sensitive(false);
    }

    if (!_fields.trackNumberMixed)
    {
      _trackNumberSpin.set_value(static_cast<double>(_fields.trackNumber));
    }
    else
    {
      _trackNumberSpin.set_value(static_cast<double>(kMixedSpinValue));
      _trackNumberSpin.set_sensitive(false);
    }

    if (!_fields.totalTracksMixed)
    {
      _totalTracksSpin.set_value(static_cast<double>(_fields.totalTracks));
    }
    else
    {
      _totalTracksSpin.set_value(static_cast<double>(kMixedSpinValue));
      _totalTracksSpin.set_sensitive(false);
    }

    if (!_fields.discNumberMixed)
    {
      _discNumberSpin.set_value(static_cast<double>(_fields.discNumber));
    }
    else
    {
      _discNumberSpin.set_value(static_cast<double>(kMixedSpinValue));
      _discNumberSpin.set_sensitive(false);
    }

    if (!_fields.totalDiscsMixed)
    {
      _totalDiscsSpin.set_value(static_cast<double>(_fields.totalDiscs));
    }
    else
    {
      _totalDiscsSpin.set_value(static_cast<double>(kMixedSpinValue));
      _totalDiscsSpin.set_sensitive(false);
    }

    _filePathLabel.set_text(uriMixed ? "Mixed" : aggregateFilePath);
    _codecLabel.set_text(codecMixed ? "Mixed" : formatCodec(aggregateCodecId));
    _sampleRateLabel.set_text(sampleRateMixed ? "Mixed" : formatSampleRate(aggregateSampleRate));
    _channelsLabel.set_text(channelsMixed ? "Mixed" : formatChannels(aggregateChannels));
    _bitDepthLabel.set_text(bitDepthMixed ? "Mixed" : formatBitDepth(aggregateBitDepth));
    _bitrateLabel.set_text(bitrateMixed ? "Mixed" : formatBitrate(aggregateBitrate));
    _durationLabel.set_text(durationMixed ? "Mixed" : formatDuration(aggregateDurationMs));
    _fileSizeLabel.set_text(fileSizeMixed ? "Mixed" : formatFileSize(aggregateFileSize));
    _modifiedLabel.set_text(mtimeMixed ? "Mixed" : formatMtime(aggregateMtime));
  }

  void TrackPropertiesDialog::onSave()
  {
    if (_trackIds.empty())
    {
      return;
    }

    auto patch = rt::MetadataPatch{};

    auto const entryValue = [](Gtk::Entry const& entry) -> std::string { return std::string{entry.get_text().raw()}; };

    // NOLINTBEGIN(aobus-readability-control-block-spacing)
    if (!_fields.titleMixed)
    {
      patch.optTitle = entryValue(_titleEntry);
    }
    if (!_fields.artistMixed)
    {
      patch.optArtist = entryValue(_artistEntry);
    }
    if (!_fields.albumArtistMixed)
    {
      patch.optAlbumArtist = entryValue(_albumArtistEntry);
    }
    if (!_fields.albumMixed)
    {
      patch.optAlbum = entryValue(_albumEntry);
    }
    if (!_fields.genreMixed)
    {
      patch.optGenre = entryValue(_genreEntry);
    }
    if (!_fields.composerMixed)
    {
      patch.optComposer = entryValue(_composerEntry);
    }
    if (!_fields.workMixed)
    {
      patch.optWork = entryValue(_workEntry);
    }
    // NOLINTEND(aobus-readability-control-block-spacing)
    if (!_fields.yearMixed)
    {
      patch.optYear = static_cast<std::uint16_t>(_yearSpin.get_value_as_int());
    }

    if (!_fields.trackNumberMixed)
    {
      patch.optTrackNumber = static_cast<std::uint16_t>(_trackNumberSpin.get_value_as_int());
    }

    if (!_fields.totalTracksMixed)
    {
      patch.optTotalTracks = static_cast<std::uint16_t>(_totalTracksSpin.get_value_as_int());
    }

    if (!_fields.discNumberMixed)
    {
      patch.optDiscNumber = static_cast<std::uint16_t>(_discNumberSpin.get_value_as_int());
    }

    if (!_fields.totalDiscsMixed)
    {
      patch.optTotalDiscs = static_cast<std::uint16_t>(_totalDiscsSpin.get_value_as_int());
    }

    auto const result = _mutation.updateMetadata(_trackIds, patch);

    if (result)
    {
      for (auto const trackId : _trackIds)
      {
        _rowCache.invalidate(trackId);
      }
    }
  }

  void TrackPropertiesDialog::applyMixedPlaceholder(Gtk::Entry& entry, bool mixed)
  {
    if (mixed)
    {
      entry.set_placeholder_text("<Multiple Values>");
      entry.set_sensitive(false);
    }
    else
    {
      entry.set_sensitive(true);
    }
  }
} // namespace ao::gtk
