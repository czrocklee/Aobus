// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "platform/linux/ui/TrackPresentation.h"

#include <ao/library/MusicLibrary.h>

#include <gtkmm.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace app::ui
{
  class TrackRowDataProvider;

  class TrackRow final : public Glib::Object
  {
  public:
    using TrackId = ao::TrackId;

    TrackId getTrackId() const { return _id; }

    Glib::ustring const& getArtist() const;
    Glib::ustring const& getAlbum() const;
    Glib::ustring const& getTitle() const { return _title; }
    Glib::ustring const& getColumnText(TrackColumn column) const;
    Glib::ustring const& getDisplayNumber() const;
    Glib::ustring const& getTags() const;
    std::chrono::milliseconds getDuration() const { return _duration; }
    TrackPresentationKeysView getPresentationKeys() const;
    std::uint64_t getResourceId() const { return _resourceId.value_or(0); }

    static Glib::RefPtr<TrackRow> create(TrackId id, TrackRowDataProvider const& provider);

    void populate(Glib::ustring title,
                  ao::DictionaryId artist,
                  ao::DictionaryId album,
                  ao::DictionaryId albumArtist,
                  ao::DictionaryId genre,
                  ao::DictionaryId composer,
                  ao::DictionaryId work,
                  Glib::ustring tags,
                  std::chrono::milliseconds duration,
                  std::uint16_t year,
                  std::uint16_t discNumber,
                  std::uint16_t totalDiscs,
                  std::uint16_t trackNumber,
                  std::optional<std::uint64_t> resourceId);

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

    Glib::ustring _title;
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
  };
} // namespace app::ui
