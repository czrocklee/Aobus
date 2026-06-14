// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/Type.h>
#include <ao/library/AudioCodec.h>
#include <ao/library/FileManifestLayout.h>
#include <ao/rt/TrackField.h>

#include <glibmm/object.h>
#include <glibmm/property.h>
#include <glibmm/propertyproxy.h>
#include <glibmm/refptr.h>
#include <glibmm/ustring.h>

#include <array>
#include <chrono>
#include <cstdint>

namespace ao::gtk
{
  class TrackRowCache;

  class TrackRowObject final : public Glib::Object
  {
  public:
    static Glib::RefPtr<TrackRowObject> create(TrackId id, TrackRowCache const& provider);

    TrackId trackId() const { return _id; }

    Glib::ustring const* stringField(rt::TrackField field) const noexcept;
    bool setStringField(rt::TrackField field, Glib::ustring const& value);

    Glib::ustring fieldText(rt::TrackField field) const;

    Glib::ustring const& tags() const { return _tags; }
    void setTags(Glib::ustring const& tags) { _tags = tags; }

    std::chrono::milliseconds duration() const { return _duration; }

    ResourceId resourceId() const { return _resourceId; }

    std::uint32_t sampleRate() const { return _sampleRate; }
    std::uint8_t channels() const { return _channels; }
    std::uint8_t bitDepth() const { return _bitDepth; }
    library::AudioCodec codec() const { return _codec; }

    std::uint16_t year() const { return _year; }
    void setYear(std::uint16_t year);

    std::uint16_t discNumber() const { return _discNumber; }
    void setDiscNumber(std::uint16_t discNumber);

    std::uint16_t discTotal() const { return _discTotal; }
    void setDiscTotal(std::uint16_t discTotal);

    std::uint16_t trackNumber() const { return _trackNumber; }
    void setTrackNumber(std::uint16_t trackNumber);

    std::uint16_t trackTotal() const { return _trackTotal; }
    void setTrackTotal(std::uint16_t trackTotal);

    std::uint16_t movementNumber() const { return _movementNumber; }
    void setMovementNumber(std::uint16_t movementNumber);

    std::uint16_t movementTotal() const { return _movementTotal; }
    void setMovementTotal(std::uint16_t movementTotal);

    std::uint32_t bitrate() const { return _bitrate; }
    std::uint64_t fileSize() const { return _fileSize; }
    std::uint64_t modifiedTime() const { return _modifiedTime; }
    library::FileStatus status() const { return _status; }

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
                  DictionaryId movement,
                  Glib::ustring const& tags,
                  std::chrono::milliseconds duration,
                  std::uint16_t year,
                  std::uint16_t discNumber,
                  std::uint16_t discTotal,
                  std::uint16_t trackNumber,
                  std::uint16_t trackTotal,
                  std::uint16_t movementNumber,
                  std::uint16_t movementTotal,
                  ResourceId resourceId,
                  std::uint32_t sampleRate,
                  std::uint8_t channels,
                  std::uint8_t bitDepth,
                  library::AudioCodec codec,
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
    std::uint16_t _discTotal = 0;
    std::uint16_t _trackNumber = 0;
    std::uint16_t _trackTotal = 0;
    std::uint16_t _movementNumber = 0;
    std::uint16_t _movementTotal = 0;
    ResourceId _resourceId{kInvalidResourceId};

    std::uint32_t _sampleRate = 0;
    std::uint8_t _channels = 0;
    std::uint8_t _bitDepth = 0;
    library::AudioCodec _codec = library::AudioCodec::Unknown;
    std::uint32_t _bitrate = 0;
    std::uint64_t _fileSize = 0;
    std::uint64_t _modifiedTime = 0;
    library::FileStatus _status = library::FileStatus::Available;

    Glib::Property<bool> _propertyPlaying;
  };
} // namespace ao::gtk
