// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/core/MusicLibrary.h>
#include <rs/fbs/Track_generated.h>

#include <gtkmm.h>

#include <cstdint>
#include <string>

class TrackRow : public Glib::Object
{
public:
  using TrackId = rs::core::MusicLibrary::TrackId;

  TrackId getTrackId() const { return _trackId; }
  void setTrackId(TrackId id) { _trackId = id; }

  Glib::ustring getArtist() const { return _artist; }
  void setArtist(Glib::ustring const& artist) { _artist = artist; }

  Glib::ustring getAlbum() const { return _album; }
  void setAlbum(Glib::ustring const& album) { _album = album; }

  Glib::ustring getTitle() const { return _title; }
  void setTitle(Glib::ustring const& title) { _title = title; }

  Glib::ustring getTags() const { return _tags; }
  void setTags(Glib::ustring const& tags) { _tags = tags; }

  std::uint64_t getResourceId() const { return _resourceId; }
  void setResourceId(std::uint64_t id) { _resourceId = id; }

  static Glib::RefPtr<TrackRow> create(TrackId id, rs::fbs::TrackT const& track);

protected:
  explicit TrackRow();

public:
  TrackId _trackId;
  Glib::ustring _artist;
  Glib::ustring _album;
  Glib::ustring _title;
  Glib::ustring _tags;
  std::uint64_t _resourceId;
};
