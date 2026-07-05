// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/AudioCodec.h>
#include <ao/CoreIds.h>
#include <ao/library/FileManifestLayout.h>
#include <ao/rt/TrackField.h>

#include <glibmm/object.h>
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

    // Display text for any field, without a by-value copy. Text-backed fields
    // return their stored slot directly; computed fields are formatted on first
    // access and memoized, so a recycled cell rebinding to this row re-reads the
    // cached string instead of re-running the formatter. Never null for a valid
    // field index. Prefer this over fieldText() on hot paths.
    Glib::ustring const* displayText(rt::TrackField field) const;

    Glib::ustring fieldText(rt::TrackField field) const;

    Glib::ustring const& tags() const { return _tags; }
    void setTags(Glib::ustring const& tags)
    {
      _tags = tags;
      invalidateComputedCache();
    }

    std::chrono::milliseconds duration() const { return _duration; }

    ResourceId resourceId() const { return _resourceId; }

    std::uint32_t sampleRate() const { return _sampleRate; }
    std::uint8_t channels() const { return _channels; }
    std::uint8_t bitDepth() const { return _bitDepth; }
    AudioCodec codec() const { return _codec; }

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

    bool isPlaying() const { return _playing; }
    void setPlaying(bool playing) { _playing = playing; }

    void populate(Glib::ustring title,
                  Glib::ustring artist,
                  Glib::ustring album,
                  Glib::ustring albumArtist,
                  Glib::ustring genre,
                  Glib::ustring composer,
                  Glib::ustring conductor,
                  Glib::ustring ensemble,
                  Glib::ustring work,
                  Glib::ustring movement,
                  Glib::ustring soloist,
                  Glib::ustring tags,
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
                  AudioCodec codec,
                  std::uint32_t bitrate,
                  std::uint64_t fileSize,
                  std::uint64_t modifiedTime,
                  library::FileStatus status = library::FileStatus::Available);

  protected:
    explicit TrackRowObject();

  private:
    // Clears every memoized computed-field string. Called by any mutator: computed
    // display values derive from the numeric/text members, so changing one of them
    // invalidates the cache wholesale. Editing is rare, so a full reset is simpler
    // and safer than a per-field dependency map.
    void invalidateComputedCache() noexcept { _computedFilled = 0; }

    TrackId _id;
    TrackRowCache const* _provider = nullptr;

    // Holds both text-backed fields (filled at populate) and lazily memoized
    // computed-field strings; mutable so displayText() can fill computed slots
    // from a const accessor. _computedFilled marks which computed slots are valid.
    mutable std::array<Glib::ustring, rt::kTrackFieldCount> _text{};
    mutable std::uint32_t _computedFilled = 0;

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
    AudioCodec _codec = AudioCodec::Unknown;
    std::uint32_t _bitrate = 0;
    std::uint64_t _fileSize = 0;
    std::uint64_t _modifiedTime = 0;
    library::FileStatus _status = library::FileStatus::Available;

    // Plain bool: do not replace with Glib::Property<bool>. Nothing subscribes
    // to per-row property notifications; now-playing highlight is driven by
    // TrackListModel's dedicated signal, and this flag is only stamped by
    // get_item_vfunc and read at bind time.
    bool _playing = false;
  };
} // namespace ao::gtk
