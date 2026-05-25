// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "ao/Type.h"
#include "ao/library/FileManifestLayout.h"
#include <ao/rt/TrackField.h>

#include <glibmm/object.h>
#include <glibmm/property.h>
#include <glibmm/propertyproxy.h>
#include <glibmm/refptr.h>
#include <glibmm/ustring.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>

namespace ao::gtk
{
  class TrackRowCache;

  class TrackRowObject final : public Glib::Object
  {
  public:
    static Glib::RefPtr<TrackRowObject> create(TrackId id, TrackRowCache const& provider);

    TrackId getTrackId() const { return _id; }

    Glib::ustring const* stringField(rt::TrackField field) const noexcept;
    bool setStringField(rt::TrackField field, Glib::ustring const& value);

    Glib::ustring getFieldText(rt::TrackField field) const;

    Glib::ustring const& getTags() const { return _tags; }
    void setTags(Glib::ustring const& tags) { _tags = tags; }

    std::chrono::milliseconds getDuration() const { return _duration; }

    std::uint64_t getResourceId() const { return _optResourceId.value_or(0); }

    std::uint32_t getSampleRate() const { return _sampleRate; }
    std::uint8_t getChannels() const { return _channels; }
    std::uint8_t getBitDepth() const { return _bitDepth; }
    std::uint16_t getCodecId() const { return _codecId; }

    std::uint16_t getYear() const { return _year; }
    void setYear(std::uint16_t year);

    std::uint16_t getDiscNumber() const { return _discNumber; }
    void setDiscNumber(std::uint16_t discNumber);

    std::uint16_t getTotalDiscs() const { return _totalDiscs; }
    void setTotalDiscs(std::uint16_t totalDiscs);

    std::uint16_t getTrackNumber() const { return _trackNumber; }
    void setTrackNumber(std::uint16_t trackNumber);

    std::uint16_t getTotalTracks() const { return _totalTracks; }
    void setTotalTracks(std::uint16_t totalTracks);

    std::uint32_t getBitrate() const { return _bitrate; }
    std::uint64_t getFileSize() const { return _fileSize; }
    std::uint64_t getModifiedTime() const { return _modifiedTime; }
    library::FileStatus getStatus() const { return _status; }

    Glib::PropertyProxy<bool> property_playing();

    bool isPlaying() const;
    void setPlaying(bool playing);

    void populate(Glib::ustring const& title,
                  DictionaryId artist,
                  DictionaryId album,
                  DictionaryId albumArtist,
                  DictionaryId genre,
                  DictionaryId composer,
                  DictionaryId work,
                  Glib::ustring const& tags,
                  std::chrono::milliseconds duration,
                  std::uint16_t year,
                  std::uint16_t discNumber,
                  std::uint16_t totalDiscs,
                  std::uint16_t trackNumber,
                  std::uint16_t totalTracks,
                  std::optional<std::uint64_t> optResourceId,
                  std::uint32_t sampleRate,
                  std::uint8_t channels,
                  std::uint8_t bitDepth,
                  std::uint16_t codecId,
                  std::uint32_t bitrate,
                  std::uint64_t fileSize,
                  std::uint64_t modifiedTime,
                  library::FileStatus status = library::FileStatus::Available);

  protected:
    explicit TrackRowObject();

  private:
    TrackId _id;
    TrackRowCache const* _provider = nullptr;

    std::array<Glib::ustring, rt::kTrackFieldCount> _text{};

    Glib::ustring _tags;

    std::chrono::milliseconds _duration{0};
    std::uint16_t _year = 0;
    std::uint16_t _discNumber = 0;
    std::uint16_t _totalDiscs = 0;
    std::uint16_t _trackNumber = 0;
    std::uint16_t _totalTracks = 0;
    std::optional<std::uint64_t> _optResourceId;

    std::uint32_t _sampleRate = 0;
    std::uint8_t _channels = 0;
    std::uint8_t _bitDepth = 0;
    std::uint16_t _codecId = 0;
    std::uint32_t _bitrate = 0;
    std::uint64_t _fileSize = 0;
    std::uint64_t _modifiedTime = 0;
    library::FileStatus _status = library::FileStatus::Available;

    Glib::Property<bool> _propertyPlaying;
  };
} // namespace ao::gtk
