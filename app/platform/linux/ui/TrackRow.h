// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "platform/linux/ui/TrackPresentation.h"

#include <rs/library/MusicLibrary.h>

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
    using TrackId = rs::TrackId;

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
                  rs::DictionaryId artist,
                  rs::DictionaryId album,
                  rs::DictionaryId albumArtist,
                  rs::DictionaryId genre,
                  rs::DictionaryId composer,
                  rs::DictionaryId work,
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

    rs::DictionaryId _artistId{0};
    rs::DictionaryId _albumId{0};
    rs::DictionaryId _albumArtistId{0};
    rs::DictionaryId _genreId{0};
    rs::DictionaryId _composerId{0};
    rs::DictionaryId _workId{0};

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
