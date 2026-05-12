// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "InspectorSidebar.h"
#include "TrackPresentation.h"
#include <ao/library/ResourceStore.h>
#include <ao/utility/Log.h>
#include <runtime/AppSession.h>
#include <runtime/LibraryMutationService.h>
#include <runtime/ProjectionTypes.h>
#include <runtime/StateTypes.h>
#include <runtime/TrackSource.h>
#include <runtime/ViewService.h>

#include <giomm/memoryinputstream.h>

#include <algorithm>
#include <iterator>
#include <set>

namespace ao::gtk
{
  namespace
  {
    constexpr int kSectionSpacing = 24;
    constexpr int kInternalPadding = 12;
    constexpr int kMetadataSpacing = 12;
    constexpr int kHeroImageSize = 280;
    constexpr float kLabelOpacity = 0.6F;
    constexpr float kHeaderOpacity = 0.5F;

    constexpr std::uint16_t kCodecIdFlac = 3;
    constexpr std::uint16_t kCodecIdMp3 = 0x55;
    constexpr double kKhzMultiplier = 1000.0;
    constexpr int kSecondsPerHour = 3600;
    constexpr int kSecondsPerMinute = 60;

    Glib::ustring formatCodecId(std::uint16_t codecId)
    {
      switch (codecId)
      {
        case kCodecIdFlac: return "FLAC";

        case kCodecIdMp3: return "MP3";

        default: return codecId == 0 ? "Unknown" : std::format("Codec (0x{:02x})", codecId);
      }
    }

    Glib::ustring formatSampleRate(std::uint32_t rate)
    {
      if (rate == 0)
      {
        return "Unknown";
      }

      if (rate % 1000 == 0)
      {
        return std::format("{} kHz", rate / 1000);
      }

      return std::format("{:.1f} kHz", rate / kKhzMultiplier);
    }

    std::string formatDuration(std::chrono::milliseconds ms)
    {
      auto const totalSeconds = std::chrono::duration_cast<std::chrono::seconds>(ms).count();
      auto const minutes = (totalSeconds % kSecondsPerHour) / kSecondsPerMinute;
      auto const seconds = totalSeconds % kSecondsPerMinute;
      if (totalSeconds >= kSecondsPerHour)
      {
        return std::format("{}:{:02}:{:02}", totalSeconds / kSecondsPerHour, minutes, seconds);
      }

      return std::format("{}:{:02}", minutes, seconds);
    }
  }

  InspectorSidebar::InspectorSidebar(ao::rt::AppSession& session, CoverArtCache& coverArtCache)
    : Gtk::Box{Gtk::Orientation::VERTICAL, 0}, _session{session}, _coverArtCache{coverArtCache}
  {
    setupUi();
  }

  InspectorSidebar::~InspectorSidebar() = default;

  void InspectorSidebar::setupUi()
  {
    add_css_class("inspector-sidebar");

    _scrolledWindow.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    _scrolledWindow.set_child(_contentBox);
    _scrolledWindow.set_expand(true);

    _contentBox.set_margin(kInternalPadding);
    _contentBox.set_spacing(kSectionSpacing);
    _contentBox.set_valign(Gtk::Align::START);

    buildHeroSection();
    buildMetadataSection();
    buildTagsSection();
    buildAudioSection();

    append(_scrolledWindow);

    updateEmptyState();
  }

  void InspectorSidebar::buildHeroSection()
  {
    _heroBox.add_css_class("hero-section");
    _heroBox.set_halign(Gtk::Align::CENTER);

    _coverImage.set_size_request(kHeroImageSize, kHeroImageSize);
    _coverImage.add_css_class("hero-cover");

    _heroBox.append(_coverImage);
    _heroBox.append(_noCoverLabel);

    _contentBox.append(_heroBox);
    _contentBox.append(_metadataBox);
    _contentBox.append(_audioBox);
    _contentBox.append(_tagEditor);

    _tagEditor.signalTagsChanged().connect(
      [this](auto const& toAdd, auto const& toRemove)
      {
        auto ids = _currentTrackIds;

        auto const result = _session.mutation().editTags(ids, toAdd, toRemove);

        if (result)
        {
          _session.sources().allTracks().notifyUpdated(ids);
        }
      });
  }

  void InspectorSidebar::buildMetadataSection()
  {
    _metadataBox.add_css_class("metadata-section");
    _metadataBox.set_spacing(kMetadataSpacing);

    auto const createEditableRow = [this](std::string const& labelText, Gtk::EditableLabel& editable)
    {
      auto* const box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
      auto* const title = Gtk::make_managed<Gtk::Label>(labelText);

      title->set_halign(Gtk::Align::START);
      title->add_css_class("property-label");
      title->set_opacity(kLabelOpacity);

      editable.set_halign(Gtk::Align::START);
      editable.set_hexpand(true);
      editable.set_vexpand(false);
      editable.add_css_class("property-editable");

      box->append(*title);
      box->append(editable);
      _metadataBox.append(*box);
    };

    createEditableRow("TITLE", _titleLabel);
    createEditableRow("ARTIST", _artistLabel);
    createEditableRow("ALBUM", _albumLabel);

    _titleLabel.property_editing().signal_changed().connect(sigc::mem_fun(*this, &InspectorSidebar::onTitleEdited));
    _artistLabel.property_editing().signal_changed().connect(sigc::mem_fun(*this, &InspectorSidebar::onArtistEdited));
    _albumLabel.property_editing().signal_changed().connect(sigc::mem_fun(*this, &InspectorSidebar::onAlbumEdited));
  }

  void InspectorSidebar::buildTagsSection()
  {
    // Handled by TagEditor
  }

  void InspectorSidebar::buildAudioSection()
  {
    _audioBox.add_css_class("audio-section");
    _audioBox.set_spacing(4);

    auto const createTechnicalRow = [](Gtk::Box& container, std::string const& labelText, Gtk::Label& valueLabel)
    {
      auto* const row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
      auto* const title = Gtk::make_managed<Gtk::Label>(labelText);

      title->set_halign(Gtk::Align::START);
      title->set_opacity(kLabelOpacity);
      title->add_css_class("technical-label");

      valueLabel.set_halign(Gtk::Align::END);
      valueLabel.set_hexpand(true);
      valueLabel.add_css_class("technical-value");

      row->append(*title);
      row->append(valueLabel);
      container.append(*row);
    };

    auto* const header = Gtk::make_managed<Gtk::Label>("AUDIO PROPERTIES");

    header->set_halign(Gtk::Align::START);
    header->set_margin_bottom(4);
    header->set_opacity(kHeaderOpacity);
    header->add_css_class("section-header");
    _audioBox.append(*header);

    createTechnicalRow(_audioBox, "Format", _formatLabel);
    createTechnicalRow(_audioBox, "Sample Rate", _sampleRateLabel);
    createTechnicalRow(_audioBox, "Channels", _channelsLabel);
    createTechnicalRow(_audioBox, "Duration", _durationLabel);
  }

  void InspectorSidebar::onTitleEdited()
  {
    if (_titleLabel.get_editing() || _currentTrackIds.empty())
    {
      return;
    }

    auto const newValue = _titleLabel.get_text().raw();

    if (newValue == "<Multiple Values>")
    {
      return;
    }

    auto const result =
      _session.mutation().updateMetadata(_currentTrackIds, ao::rt::MetadataPatch{.optTitle = newValue});

    if (!result)
    {
      APP_LOG_ERROR("Failed to update titles: {}", result.error().message);

      if (_detailProjection)
      {
        auto const snap = _detailProjection->snapshot();
        _titleLabel.set_text(snap.title.mixed ? "<Multiple Values>" : snap.title.optValue.value_or(""));
      }
    }
  }

  void InspectorSidebar::onArtistEdited()
  {
    if (_artistLabel.get_editing() || _currentTrackIds.empty())
    {
      return;
    }

    auto const newValue = _artistLabel.get_text().raw();

    if (newValue == "<Multiple Values>")
    {
      return;
    }

    auto const result =
      _session.mutation().updateMetadata(_currentTrackIds, ao::rt::MetadataPatch{.optArtist = newValue});

    if (!result)
    {
      APP_LOG_ERROR("Failed to update artists: {}", result.error().message);
      if (_detailProjection)
      {
        auto const snap = _detailProjection->snapshot();
        _artistLabel.set_text(snap.artist.mixed ? "<Multiple Values>" : snap.artist.optValue.value_or(""));
      }
    }
  }

  void InspectorSidebar::onAlbumEdited()
  {
    if (_albumLabel.get_editing() || _currentTrackIds.empty())
    {
      return;
    }

    auto const newValue = _albumLabel.get_text().raw();

    if (newValue == "<Multiple Values>")
    {
      return;
    }

    auto const result =
      _session.mutation().updateMetadata(_currentTrackIds, ao::rt::MetadataPatch{.optAlbum = newValue});

    if (!result)
    {
      APP_LOG_ERROR("Failed to update albums: {}", result.error().message);
      if (_detailProjection)
      {
        auto const snap = _detailProjection->snapshot();
        _albumLabel.set_text(snap.album.mixed ? "<Multiple Values>" : snap.album.optValue.value_or(""));
      }
    }
  }

  void InspectorSidebar::updateEmptyState()
  {
    _heroBox.set_visible(false);
    _metadataBox.set_visible(false);
    _audioBox.set_visible(false);
    _tagEditor.set_visible(false);
  }

  void InspectorSidebar::bindToDetailProjection(std::shared_ptr<ao::rt::ITrackDetailProjection> projection)
  {
    _detailProjection = std::move(projection);
    _detailSub =
      _detailProjection->subscribe([this](ao::rt::TrackDetailSnapshot const& snap) { onTrackDetailSnapshot(snap); });
  }

  void InspectorSidebar::onTrackDetailSnapshot(ao::rt::TrackDetailSnapshot const& snap)
  {
    _currentTrackIds = snap.trackIds;

    if (snap.selectionKind == ao::rt::SelectionKind::None)
    {
      updateEmptyState();
      return;
    }

    _heroBox.set_visible(true);
    _metadataBox.set_visible(true);

    _titleLabel.set_text(snap.title.mixed ? "<Multiple Values>" : snap.title.optValue.value_or(""));
    _artistLabel.set_text(snap.artist.mixed ? "<Multiple Values>" : snap.artist.optValue.value_or(""));
    _albumLabel.set_text(snap.album.mixed ? "<Multiple Values>" : snap.album.optValue.value_or(""));

    updateCoverArt(snap);
    updateAudioMetadata(snap);

    // Tags
    _tagEditor.setup(_session.musicLibrary(), std::vector<ao::TrackId>{_currentTrackIds});
    _tagEditor.set_visible(true);
  }

  void InspectorSidebar::updateCoverArt(ao::rt::TrackDetailSnapshot const& snap)
  {
    if (snap.singleCoverArtId == ao::ResourceId{0})
    {
      _coverImage.set_visible(false);
      _noCoverLabel.set_visible(true);
      return;
    }

    auto pixbuf = _coverArtCache.get(static_cast<std::uint64_t>(snap.singleCoverArtId.value()));
    if (!pixbuf)
    {
      pixbuf = loadCoverArtFromLibrary(snap.singleCoverArtId);
      if (pixbuf)
      {
        _coverArtCache.put(static_cast<std::uint64_t>(snap.singleCoverArtId.value()), pixbuf);
      }
    }

    if (pixbuf)
    {
      _coverImage.set_pixbuf(pixbuf);
      _coverImage.set_visible(true);
      _noCoverLabel.set_visible(false);
    }
  }

  Glib::RefPtr<Gdk::Pixbuf> InspectorSidebar::loadCoverArtFromLibrary(ao::ResourceId resourceId)
  {
    auto const txn = _session.musicLibrary().readTransaction();
    auto const reader = _session.musicLibrary().resources().reader(txn);
    auto const data = reader.get(static_cast<std::uint32_t>(resourceId.value()));

    if (!data)
    {
      return {};
    }

    try
    {
      auto const memStream = Gio::MemoryInputStream::create();
      memStream->add_data(data->data(), static_cast<gssize>(data->size()), nullptr);
      return Gdk::Pixbuf::create_from_stream(memStream);
    }
    catch (std::exception const& ex)
    {
      APP_LOG_ERROR("Failed to load cover art: {}", ex.what());
      return {};
    }
  }

  void InspectorSidebar::updateAudioMetadata(ao::rt::TrackDetailSnapshot const& snap)
  {
    _audioBox.set_visible(snap.selectionKind == ao::rt::SelectionKind::Single);

    auto const setLabel = [](Gtk::Label& label, auto const& property, auto const& formatter)
    {
      if (property.optValue && !property.mixed)
      {
        label.set_text(formatter(*property.optValue));
      }
      else
      {
        label.set_text(property.mixed ? "Mixed" : "Unknown");
      }
    };

    setLabel(_formatLabel, snap.audio.codecId, [](auto id) { return formatCodecId(id); });
    setLabel(_sampleRateLabel, snap.audio.sampleRate, [](auto rate) { return formatSampleRate(rate); });
    setLabel(_channelsLabel, snap.audio.channels, [](auto ch) { return std::format("{} Ch", ch); });
    setLabel(
      _durationLabel, snap.audio.durationMs, [](auto ms) { return formatDuration(std::chrono::milliseconds{ms}); });
  }
} // namespace ao::gtk
