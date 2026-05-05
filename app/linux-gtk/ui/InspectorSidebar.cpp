// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "InspectorSidebar.h"
#include "LibrarySession.h"
#include "TrackPresentation.h"
#include <ao/library/ResourceStore.h>
#include <ao/utility/Log.h>

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
  }

  InspectorSidebar::InspectorSidebar(MetadataCoordinator& metadataCoordinator, CoverArtCache& coverArtCache)
    : Gtk::Box{Gtk::Orientation::VERTICAL, 0}, _metadataCoordinator{metadataCoordinator}, _coverArtCache{coverArtCache}
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
        auto const spec = MetadataCoordinator::MetadataUpdateSpec{.tagsToAdd = toAdd, .tagsToRemove = toRemove};

        auto ids = _currentSelection | std::views::transform([](auto const& row) { return row->getTrackId(); }) |
                   std::ranges::to<std::vector>();

        auto const idsCopy = ids;
        _metadataCoordinator.updateMetadata(_currentSession->musicLibrary.get(),
                                            std::move(ids),
                                            spec,
                                            [this, ids = std::move(idsCopy)](auto const& result)
                                            {
                                              if (result)
                                              {
                                                for (auto const trackId : ids)
                                                {
                                                  _currentSession->rowDataProvider->invalidate(trackId);
                                                }

                                                _currentSession->allTrackIds->notifyUpdated(ids);
                                              }
                                            });
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

  void InspectorSidebar::updateSelection(LibrarySession* session, std::vector<Glib::RefPtr<TrackRow>> const& rows)
  {
    _currentSession = session;
    _currentSelection = rows;

    if (rows.empty())
    {
      updateEmptyState();
      return;
    }

    if (rows.size() == 1)
    {
      updateSingleSelection(session, rows[0]);
    }
    else
    {
      updateMultiSelection(session, rows);
    }
  }

  void InspectorSidebar::updateEmptyState()
  {
    _heroBox.set_visible(false);
    _metadataBox.set_visible(false);
    _audioBox.set_visible(false);
    _tagEditor.set_visible(false);
  }

  void InspectorSidebar::updateSingleSelection(LibrarySession* session, Glib::RefPtr<TrackRow> const& row)
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

      if (!pixbuf && session != nullptr)
      {
        auto txn = session->musicLibrary->readTransaction();
        auto const reader = session->musicLibrary->resources().reader(txn);
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
    _tagEditor.setup(*session->musicLibrary, std::move(ids));
    _tagEditor.set_visible(true);
  }

  void InspectorSidebar::updateMultiSelection(LibrarySession* /*session*/,
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

    _tagEditor.setup(*_currentSession->musicLibrary, std::move(ids));
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

    auto const spec = MetadataCoordinator::MetadataUpdateSpec{.title = newValue};

    auto ids = _currentSelection | std::views::transform([](auto const& row) { return row->getTrackId(); }) |
               std::ranges::to<std::vector>();

    struct RollbackEntry
    {
      Glib::RefPtr<TrackRow> row;
      std::string oldTitle;
    };

    auto rollbackData = std::vector<RollbackEntry>{};
    rollbackData.reserve(_currentSelection.size());

    for (auto const& row : _currentSelection)
    {
      rollbackData.push_back({.row = row, .oldTitle = row->getTitle()});

      // Optimistic update
      row->setTitle(newValue);
    }

    _metadataCoordinator.updateMetadata(_currentSession->musicLibrary.get(),
                                        std::move(ids),
                                        spec,
                                        [rollbackData = std::move(rollbackData)](auto const& result)
                                        {
                                          if (!result)
                                          {
                                            APP_LOG_ERROR("Failed to update titles: {}", result.error().message);

                                            for (auto const& entry : rollbackData)
                                            {
                                              entry.row->setTitle(entry.oldTitle);
                                            }
                                          }
                                        });
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

    auto const spec = MetadataCoordinator::MetadataUpdateSpec{.artist = newValue};

    auto ids = _currentSelection | std::views::transform([](auto const& row) { return row->getTrackId(); }) |
               std::ranges::to<std::vector>();

    struct RollbackEntry
    {
      Glib::RefPtr<TrackRow> row;
      std::string oldArtist;
    };

    auto rollbackData = std::vector<RollbackEntry>{};
    rollbackData.reserve(_currentSelection.size());

    for (auto const& row : _currentSelection)
    {
      rollbackData.push_back({.row = row, .oldArtist = row->getArtist()});

      // Optimistic update
      row->setArtist(newValue);
    }

    _metadataCoordinator.updateMetadata(_currentSession->musicLibrary.get(),
                                        std::move(ids),
                                        spec,
                                        [rollbackData = std::move(rollbackData)](auto const& result)
                                        {
                                          if (!result)
                                          {
                                            APP_LOG_ERROR("Failed to update artists: {}", result.error().message);

                                            for (auto const& entry : rollbackData)
                                            {
                                              entry.row->setArtist(entry.oldArtist);
                                            }
                                          }
                                        });
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

    auto const spec = MetadataCoordinator::MetadataUpdateSpec{.album = newValue};

    auto ids = _currentSelection | std::views::transform([](auto const& row) { return row->getTrackId(); }) |
               std::ranges::to<std::vector>();

    struct RollbackEntry
    {
      Glib::RefPtr<TrackRow> row;
      std::string oldAlbum;
    };

    auto rollbackData = std::vector<RollbackEntry>{};
    rollbackData.reserve(_currentSelection.size());

    for (auto const& row : _currentSelection)
    {
      rollbackData.push_back({.row = row, .oldAlbum = row->getAlbum()});

      // Optimistic update
      row->setAlbum(newValue);
    }

    _metadataCoordinator.updateMetadata(_currentSession->musicLibrary.get(),
                                        std::move(ids),
                                        spec,
                                        [rollbackData = std::move(rollbackData)](auto const& result)
                                        {
                                          if (!result)
                                          {
                                            APP_LOG_ERROR("Failed to update albums: {}", result.error().message);

                                            for (auto const& entry : rollbackData)
                                            {
                                              entry.row->setAlbum(entry.oldAlbum);
                                            }
                                          }
                                        });
  }
} // namespace ao::gtk
