// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "TrackPresentation.h"

#include <ao/library/MusicLibrary.h>

#include <gtkmm.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace ao::gtk
{
  class TrackRowDataProvider;

  class TrackRow final : public Glib::Object
  {
  public:
    using TrackId = ao::TrackId;

    static Glib::RefPtr<TrackRow> create(TrackId id, TrackRowDataProvider const& provider);

    TrackId getTrackId() const { return _id; }

    Glib::ustring getArtist() const { return _propertyArtist.get_value(); }
    void setArtist(Glib::ustring const& artist);
    Glib::PropertyProxy<Glib::ustring> property_artist();

    Glib::ustring getAlbum() const { return _propertyAlbum.get_value(); }
    void setAlbum(Glib::ustring const& album);
    Glib::PropertyProxy<Glib::ustring> property_album();

    Glib::ustring getTitle() const { return _propertyTitle.get_value(); }
    void setTitle(Glib::ustring const& title);
    Glib::PropertyProxy<Glib::ustring> property_title();

    Glib::ustring getColumnText(TrackColumn column) const;
    Glib::ustring const& getDisplayNumber() const;
    Glib::ustring const& getTags() const;
    void setTags(Glib::ustring const& tags);
    std::chrono::milliseconds getDuration() const { return _duration; }
    Glib::ustring const& getDurationStr() const { return _durationStr; }
    std::uint64_t getResourceId() const { return _resourceId.value_or(0); }

    std::uint32_t getSampleRate() const { return _sampleRate; }
    std::uint8_t getChannels() const { return _channels; }
    std::uint8_t getBitDepth() const { return _bitDepth; }
    std::uint16_t getCodecId() const { return _codecId; }

    Glib::PropertyProxy<bool> property_playing();
    bool isPlaying() const;
    void setPlaying(bool playing);

    void populate(Glib::ustring const& title,
                  ao::DictionaryId artist,
                  ao::DictionaryId album,
                  ao::DictionaryId albumArtist,
                  ao::DictionaryId genre,
                  ao::DictionaryId composer,
                  ao::DictionaryId work,
                  Glib::ustring const& tags,
                  std::chrono::milliseconds duration,
                  std::uint16_t year,
                  std::uint16_t discNumber,
                  std::uint16_t totalDiscs,
                  std::uint16_t trackNumber,
                  std::optional<std::uint64_t> resourceId,
                  std::uint32_t sampleRate,
                  std::uint8_t channels,
                  std::uint8_t bitDepth,
                  std::uint16_t codecId);

  protected:
    explicit TrackRow();

  private:
    TrackId _id;
    TrackRowDataProvider const* _provider = nullptr;

    ao::DictionaryId _artistId{0};
    ao::DictionaryId _albumId{0};
    ao::DictionaryId _albumArtistId{0};
    ao::DictionaryId _genreId{0};
    ao::DictionaryId _composerId{0};
    ao::DictionaryId _workId{0};

    Glib::ustring _tags;
    Glib::ustring _yearStr;
    Glib::ustring _discNumberStr;
    Glib::ustring _trackNumberStr;
    Glib::ustring _displayNumberStr;
    Glib::ustring _durationStr;

    std::chrono::milliseconds _duration{0};
    std::uint16_t _year = 0;
    std::uint16_t _discNumber = 0;
    std::uint16_t _totalDiscs = 0;
    std::uint16_t _trackNumber = 0;
    std::optional<std::uint64_t> _resourceId;

    std::uint32_t _sampleRate = 0;
    std::uint8_t _channels = 0;
    std::uint8_t _bitDepth = 0;
    std::uint16_t _codecId = 0;

    Glib::Property<bool> _propertyPlaying;
    Glib::Property<Glib::ustring> _propertyTitle;
    Glib::Property<Glib::ustring> _propertyArtist;
    Glib::Property<Glib::ustring> _propertyAlbum;
  };
} // namespace ao::gtk
