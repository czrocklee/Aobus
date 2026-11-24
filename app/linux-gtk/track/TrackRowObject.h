// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/AudioCodec.h>
#include <ao/CoreIds.h>
#include <ao/FileTimestamp.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/library/FileManifestLayout.h>
#include <ao/library/RecordingDate.h>
#include <ao/rt/TrackField.h>

#include <glibmm/object.h>
#include <glibmm/refptr.h>
#include <glibmm/ustring.h>
#include <sigc++/signal.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>

namespace ao::rt
{
  struct TrackRow;
}

namespace ao::gtk
{
  class TrackRowCache;

  class TrackRowObject final : public Glib::Object
  {
  public:
    static Glib::RefPtr<TrackRowObject> create(TrackId id, i18n::MessageCatalog const& textCatalog);

    TrackId trackId() const { return _id; }

    Glib::ustring const* stringField(rt::TrackField field) const noexcept;

    // Display text for any field, without a by-value copy. Text-backed fields
    // return their stored slot directly; computed fields are formatted on first
    // access and memoized, so a recycled cell rebinding to this row re-reads the
    // cached string instead of re-running the formatter. Never null for a valid
    // field index. Prefer this over fieldText() on hot paths.
    Glib::ustring const* displayText(rt::TrackField field) const;

    Glib::ustring fieldText(rt::TrackField field) const;
    i18n::MessageCatalog const& textCatalog() const;

    Glib::ustring const& tags() const { return _tags; }

    std::chrono::milliseconds duration() const { return _duration; }

    std::uint32_t sampleRate() const { return _sampleRate; }
    std::uint8_t channels() const { return _channels; }
    std::uint8_t bitDepth() const { return _bitDepth; }
    AudioCodec codec() const { return _codec; }

    std::uint16_t year() const { return _year; }
    library::RecordingDate recordingDate() const { return _recordingDate; }
    std::uint16_t discNumber() const { return _discNumber; }
    std::uint16_t discTotal() const { return _discTotal; }
    std::uint16_t trackNumber() const { return _trackNumber; }
    std::uint16_t trackTotal() const { return _trackTotal; }
    std::uint16_t movementNumber() const { return _movementNumber; }
    std::uint16_t movementTotal() const { return _movementTotal; }

    std::uint32_t bitrate() const { return _bitrate; }
    std::uint64_t fileSize() const { return _fileSize; }
    std::optional<FileTimestamp> modifiedTime() const { return _optModifiedTime; }
    library::FileStatus status() const { return _status; }

    // Emitted once after populate has replaced all fields and invalidated computed text.
    sigc::signal<void()>& signalFieldsChanged() noexcept { return _fieldsChanged; }

    // Only a bound FilePath cell connects here. Its scoped connection keeps
    // requesting path resolution when this shared row is refreshed.
    sigc::signal<void()>& signalFilePathChanged() noexcept { return _filePathChanged; }

    void populate(rt::TrackRow row);

  protected:
    explicit TrackRowObject(i18n::MessageCatalog textCatalog);

  private:
    friend class TrackRowCache;

    void setFilePathText(rt::TrackRow const& row);
    void updateFilePath(rt::TrackRow const& row);

    TrackId _id;
    i18n::MessageCatalog _textCatalog;

    // Holds both text-backed fields (filled at populate) and lazily memoized
    // computed-field strings; mutable so displayText() can fill computed slots
    // from a const accessor. _computedFilled marks which computed slots are valid.
    mutable std::array<Glib::ustring, rt::kTrackFieldCount> _text{};
    mutable std::uint32_t _computedFilled = 0;

    Glib::ustring _tags;
    std::array<Glib::ustring, 3> _creditSummaries;

    std::chrono::milliseconds _duration{0};
    std::uint16_t _year = 0;
    library::RecordingDate _recordingDate{};
    std::uint16_t _discNumber = 0;
    std::uint16_t _discTotal = 0;
    std::uint16_t _trackNumber = 0;
    std::uint16_t _trackTotal = 0;
    std::uint16_t _movementNumber = 0;
    std::uint16_t _movementTotal = 0;

    std::uint32_t _sampleRate = 0;
    std::uint8_t _channels = 0;
    std::uint8_t _bitDepth = 0;
    AudioCodec _codec = AudioCodec::Unknown;
    std::uint32_t _bitrate = 0;
    std::uint64_t _fileSize = 0;
    std::optional<FileTimestamp> _optModifiedTime{};
    library::FileStatus _status = library::FileStatus::Available;

    sigc::signal<void()> _fieldsChanged;
    sigc::signal<void()> _filePathChanged;
  };
} // namespace ao::gtk
