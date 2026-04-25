// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "platform/linux/ui/TrackPresentation.h"

#include <rs/core/MusicLibrary.h>

#include <gtkmm.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace app::core::model
{
  class TrackRowDataProvider;
}

namespace app::ui
{

  class TrackRow final : public Glib::Object
  {
  public:
    using TrackId = rs::core::TrackId;

    TrackId getTrackId() const { return _id; }

    const Glib::ustring& getArtist() const;
    const Glib::ustring& getAlbum() const;
    const Glib::ustring& getTitle() const;
    const Glib::ustring& getColumnText(TrackColumn column) const;
    const Glib::ustring& getDisplayNumber() const;
    const Glib::ustring& getTags() const;
    std::chrono::milliseconds getDuration() const;
    TrackPresentationKeysView getPresentationKeys() const;
    std::uint64_t getResourceId() const;

    void ensureLoaded() const;

    static Glib::RefPtr<TrackRow> create(TrackId id, std::shared_ptr<app::core::model::TrackRowDataProvider> provider);

  protected:
    explicit TrackRow();

  private:
    TrackId _id;
    std::shared_ptr<app::core::model::TrackRowDataProvider> _provider;
    mutable bool _loaded = false;
    mutable Glib::ustring _artist;
    mutable Glib::ustring _album;
    mutable Glib::ustring _albumArtist;
    mutable Glib::ustring _genre;
    mutable Glib::ustring _composer;
    mutable Glib::ustring _work;
    mutable Glib::ustring _title;
    mutable Glib::ustring _tags;
    mutable Glib::ustring _yearStr;
    mutable Glib::ustring _discNumberStr;
    mutable Glib::ustring _trackNumberStr;
    mutable Glib::ustring _displayNumberStr;
    mutable Glib::ustring _durationStr;
    mutable std::chrono::milliseconds _duration{0};
    mutable std::uint16_t _year = 0;
    mutable std::uint16_t _discNumber = 0;
    mutable std::uint16_t _totalDiscs = 0;
    mutable std::uint16_t _trackNumber = 0;
    mutable std::optional<std::uint64_t> _resourceId;
  };

} // namespace app::ui
