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

  static Glib::RefPtr<TrackRow> create(TrackId id, std::shared_ptr<app::core::model::TrackRowDataProvider> provider);

  TrackId getTrackId() const { return _id; }

  Glib::ustring getArtist() const;
  Glib::ustring getAlbum() const;
  Glib::ustring getTitle() const;
  Glib::ustring getDisplayNumber() const;
  Glib::ustring getTags() const;
  std::chrono::milliseconds getDuration() const;
  TrackPresentationKeysView getPresentationKeys() const;
  std::uint64_t getResourceId() const;

  void ensureLoaded() const;

protected:
  explicit TrackRow();

private:
  TrackId _id;
  std::shared_ptr<app::core::model::TrackRowDataProvider> _provider;
  mutable bool _loaded = false;
  mutable std::string _artist;
  mutable std::string _album;
  mutable std::string _albumArtist;
  mutable std::string _genre;
  mutable std::string _title;
  mutable std::string _tags;
  mutable std::chrono::milliseconds _duration{0};
  mutable std::uint16_t _year = 0;
  mutable std::uint16_t _discNumber = 0;
  mutable std::uint16_t _totalDiscs = 0;
  mutable std::uint16_t _trackNumber = 0;
  mutable std::optional<std::uint64_t> _resourceId;
};

} // namespace app::ui
