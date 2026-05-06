// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "InspectorSidebar.h"
#include "TrackPresentation.h"
#include "TrackRowDataProvider.h"
#include <ao/library/ResourceStore.h>
#include <ao/model/TrackIdList.h>
#include <ao/utility/Log.h>
#include <runtime/AppSession.h>
#include <runtime/CommandTypes.h>
#include <runtime/ProjectionTypes.h>

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
      auto const minutes = (totalSeconds % 3600) / 60;
      auto const seconds = totalSeconds % 60;
      if (totalSeconds >= 3600)
      {
        return std::format("{}:{:02}:{:02}", totalSeconds / 3600, minutes, seconds);
      }
      return std::format("{}:{:02}", minutes, seconds);
    }
  }

  InspectorSidebar::InspectorSidebar(ao::app::AppSession& session, CoverArtCache& coverArtCache)
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

    _tagEditor.signal_tags_changed().connect(
      [this](auto const& toAdd, auto const& toRemove)
      {
        auto ids = _currentSelection | std::views::transform([](auto const& row) { return row->getTrackId(); }) |
                   std::ranges::to<std::vector>();

        auto const result = _session.commands().execute<ao::app::EditTrackTags>(ao::app::EditTrackTags{
          .trackIds = ids,
          .tagsToAdd = toAdd,
          .tagsToRemove = toRemove,
        });

        if (result)
        {
          if (_dataProvider)
          {
            for (auto const trackId : ids)
            {
              _dataProvider->invalidate(trackId);
            }
          }
          _session.allTracks().notifyUpdated(ids);
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

  void InspectorSidebar::updateSelection(TrackRowDataProvider* dataProvider,
                                         std::vector<Glib::RefPtr<TrackRow>> const& rows)
  {
    _dataProvider = dataProvider;
    _currentSelection = rows;

    if (rows.empty())
    {
      updateEmptyState();
      return;
    }

    if (rows.size() == 1)
    {
      updateSingleSelection(dataProvider, rows[0]);
    }
    else
    {
      updateMultiSelection(dataProvider, rows);
    }
  }

  void InspectorSidebar::updateEmptyState()
  {
    _heroBox.set_visible(false);
    _metadataBox.set_visible(false);
    _audioBox.set_visible(false);
    _tagEditor.set_visible(false);
  }

  void InspectorSidebar::updateSingleSelection(TrackRowDataProvider* dataProvider, Glib::RefPtr<TrackRow> const& row)
  {
    _heroBox.set_visible(true);
    _metadataBox.set_visible(true);

    _titleLabel.set_text(row->getTitle());
    _artistLabel.set_text(row->getArtist());
    _albumLabel.set_text(row->getAlbum());

    auto const resourceId = row->getResourceId();
    auto coverShown = false;

    if (resourceId != 0)
    {
      auto pixbuf = _coverArtCache.get(resourceId);

      if (!pixbuf)
      {
        auto txn = _session.musicLibrary().readTransaction();
        auto const reader = _session.musicLibrary().resources().reader(txn);
        auto const data = reader.get(static_cast<std::uint32_t>(resourceId));

        if (data)
        {
          try
          {
            auto const memStream = Gio::MemoryInputStream::create();
            memStream->add_data(data->data(), static_cast<gssize>(data->size()), nullptr);

            pixbuf = Gdk::Pixbuf::create_from_stream(memStream);

            if (pixbuf)
            {
              _coverArtCache.put(resourceId, pixbuf);
            }
          }
          catch (std::exception const& ex)
          {
            APP_LOG_ERROR("Failed to load cover art: {}", ex.what());
          }
        }
      }

      if (pixbuf)
      {
        _coverImage.set_pixbuf(pixbuf);
        _coverImage.set_visible(true);
        _noCoverLabel.set_visible(false);
        coverShown = true;
      }
    }

    if (!coverShown)
    {
      _coverImage.set_visible(false);
      _noCoverLabel.set_visible(true);
    }

    // Audio properties
    _formatLabel.set_text(formatCodecId(row->getCodecId()));
    _sampleRateLabel.set_text(formatSampleRate(row->getSampleRate()));
    _channelsLabel.set_text(std::format("{} Ch", row->getChannels()));
    _durationLabel.set_text(row->getDurationStr());
    _audioBox.set_visible(true);

    // Tags
    auto ids = std::vector<ao::TrackId>{row->getTrackId()};
    _tagEditor.setup(_session.musicLibrary(), std::move(ids));
    _tagEditor.set_visible(true);
  }

  void InspectorSidebar::updateMultiSelection(TrackRowDataProvider* /*dataProvider*/,
                                              std::vector<Glib::RefPtr<TrackRow>> const& rows)
  {
    _heroBox.set_visible(true);
    _metadataBox.set_visible(true);

    if (rows.empty())
    {
      return;
    }

    auto aggregated = AggregatedMetadata{
      .title = rows[0]->getTitle().raw(), .artist = rows[0]->getArtist().raw(), .album = rows[0]->getAlbum().raw()};

    for (std::size_t i = 1; i < rows.size(); ++i)
    {
      if (aggregated.sameTitle && rows[i]->getTitle().raw() != aggregated.title)
      {
        aggregated.sameTitle = false;
      }

      if (aggregated.sameArtist && rows[i]->getArtist().raw() != aggregated.artist)
      {
        aggregated.sameArtist = false;
      }

      if (aggregated.sameAlbum && rows[i]->getAlbum().raw() != aggregated.album)
      {
        aggregated.sameAlbum = false;
      }
    }

    _titleLabel.set_text(aggregated.sameTitle ? aggregated.title : "<Multiple Values>");
    _artistLabel.set_text(aggregated.sameArtist ? aggregated.artist : "<Multiple Values>");
    _albumLabel.set_text(aggregated.sameAlbum ? aggregated.album : "<Multiple Values>");

    _coverImage.set_visible(false);
    _noCoverLabel.set_visible(true);
    _audioBox.set_visible(false);

    auto ids =
      rows | std::views::transform([](auto const& row) { return row->getTrackId(); }) | std::ranges::to<std::vector>();

    _tagEditor.setup(_session.musicLibrary(), std::move(ids));
    _tagEditor.set_visible(true);
  }

  void InspectorSidebar::onTitleEdited()
  {
    if (_titleLabel.get_editing() || _currentSelection.empty())
    {
      return;
    }

    auto const newValue = _titleLabel.get_text().raw();

    if (newValue == "<Multiple Values>")
    {
      return;
    }

    auto ids = _currentSelection | std::views::transform([](auto const& row) { return row->getTrackId(); }) |
               std::ranges::to<std::vector>();

    auto rollbackData = std::vector<Glib::RefPtr<TrackRow>>{};
    auto oldTitles = std::vector<std::string>{};
    rollbackData.reserve(_currentSelection.size());
    oldTitles.reserve(_currentSelection.size());

    for (auto const& row : _currentSelection)
    {
      oldTitles.push_back(row->getTitle());
      rollbackData.push_back(row);
      row->setTitle(newValue);
    }

    auto const result = _session.commands().execute<ao::app::UpdateTrackMetadata>(ao::app::UpdateTrackMetadata{
      .trackIds = ids,
      .patch = ao::app::MetadataPatch{.optTitle = newValue},
    });

    if (!result)
    {
      APP_LOG_ERROR("Failed to update titles: {}", result.error().message);
      for (std::size_t i = 0; i < rollbackData.size(); ++i)
      {
        rollbackData[i]->setTitle(oldTitles[i]);
      }
    }
  }

  void InspectorSidebar::onArtistEdited()
  {
    if (_artistLabel.get_editing() || _currentSelection.empty())
    {
      return;
    }

    auto const newValue = _artistLabel.get_text().raw();

    if (newValue == "<Multiple Values>")
    {
      return;
    }

    auto ids = _currentSelection | std::views::transform([](auto const& row) { return row->getTrackId(); }) |
               std::ranges::to<std::vector>();

    auto rollbackRows = std::vector<Glib::RefPtr<TrackRow>>{};
    auto oldValues = std::vector<std::string>{};
    rollbackRows.reserve(_currentSelection.size());
    oldValues.reserve(_currentSelection.size());

    for (auto const& row : _currentSelection)
    {
      oldValues.push_back(row->getArtist());
      rollbackRows.push_back(row);
      row->setArtist(newValue);
    }

    auto const result = _session.commands().execute<ao::app::UpdateTrackMetadata>(ao::app::UpdateTrackMetadata{
      .trackIds = ids,
      .patch = ao::app::MetadataPatch{.optArtist = newValue},
    });

    if (!result)
    {
      APP_LOG_ERROR("Failed to update artists: {}", result.error().message);
      for (std::size_t i = 0; i < rollbackRows.size(); ++i)
      {
        rollbackRows[i]->setArtist(oldValues[i]);
      }
    }
  }

  void InspectorSidebar::onAlbumEdited()
  {
    if (_albumLabel.get_editing() || _currentSelection.empty())
    {
      return;
    }

    auto const newValue = _albumLabel.get_text().raw();

    if (newValue == "<Multiple Values>")
    {
      return;
    }

    auto ids = _currentSelection | std::views::transform([](auto const& row) { return row->getTrackId(); }) |
               std::ranges::to<std::vector>();

    auto rollbackRows = std::vector<Glib::RefPtr<TrackRow>>{};
    auto oldValues = std::vector<std::string>{};
    rollbackRows.reserve(_currentSelection.size());
    oldValues.reserve(_currentSelection.size());

    for (auto const& row : _currentSelection)
    {
      oldValues.push_back(row->getAlbum());
      rollbackRows.push_back(row);
      row->setAlbum(newValue);
    }

    auto const result = _session.commands().execute<ao::app::UpdateTrackMetadata>(ao::app::UpdateTrackMetadata{
      .trackIds = ids,
      .patch = ao::app::MetadataPatch{.optAlbum = newValue},
    });

    if (!result)
    {
      APP_LOG_ERROR("Failed to update albums: {}", result.error().message);
      for (std::size_t i = 0; i < rollbackRows.size(); ++i)
      {
        rollbackRows[i]->setAlbum(oldValues[i]);
      }
    }
  }
  void InspectorSidebar::bindToDetailProjection(std::shared_ptr<ao::app::ITrackDetailProjection> projection)
  {
    _detailProjection = std::move(projection);
    _detailSub = _detailProjection->subscribe(
      [this](ao::app::TrackDetailSnapshot const& snap)
      {
        if (snap.selectionKind == ao::app::SelectionKind::None)
        {
          updateEmptyState();
          return;
        }

        _heroBox.set_visible(true);
        _audioBox.set_visible(true);

        // Cover art from projection
        if (snap.singleCoverArtId != ao::ResourceId{0})
        {
          auto pixbuf = _coverArtCache.get(static_cast<std::uint64_t>(snap.singleCoverArtId.value()));
          if (pixbuf)
          {
            _coverImage.set_pixbuf(pixbuf);
            _coverImage.set_visible(true);
            _noCoverLabel.set_visible(false);
          }
        }

        // Audio properties from projection
        if (snap.audio.codecId.optValue && !snap.audio.codecId.mixed)
        {
          _formatLabel.set_text(formatCodecId(*snap.audio.codecId.optValue));
        }
        else
        {
          _formatLabel.set_text(snap.audio.codecId.mixed ? "Mixed" : "Unknown");
        }

        if (snap.audio.sampleRate.optValue && !snap.audio.sampleRate.mixed)
        {
          _sampleRateLabel.set_text(formatSampleRate(*snap.audio.sampleRate.optValue));
        }
        else
        {
          _sampleRateLabel.set_text(snap.audio.sampleRate.mixed ? "Mixed" : "Unknown");
        }

        if (snap.audio.channels.optValue && !snap.audio.channels.mixed)
        {
          _channelsLabel.set_text(std::format("{} Ch", *snap.audio.channels.optValue));
        }
        else
        {
          _channelsLabel.set_text(snap.audio.channels.mixed ? "Mixed" : "Unknown");
        }

        if (snap.audio.durationMs.optValue && !snap.audio.durationMs.mixed)
        {
          auto const durationMs = std::chrono::milliseconds{*snap.audio.durationMs.optValue};
          _durationLabel.set_text(formatDuration(durationMs));
        }
        else
        {
          _durationLabel.set_text(snap.audio.durationMs.mixed ? "Mixed" : "Unknown");
        }
      });
  }
} // namespace ao::gtk
