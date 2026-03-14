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
  void setArtist(const Glib::ustring& artist) { _artist = artist; }

  Glib::ustring getAlbum() const { return _album; }
  void setAlbum(const Glib::ustring& album) { _album = album; }

  Glib::ustring getTitle() const { return _title; }
  void setTitle(const Glib::ustring& title) { _title = title; }

  Glib::ustring getTags() const { return _tags; }
  void setTags(const Glib::ustring& tags) { _tags = tags; }

  std::uint64_t getResourceId() const { return _resourceId; }
  void setResourceId(std::uint64_t id) { _resourceId = id; }

  static Glib::RefPtr<TrackRow> create(TrackId id, const rs::fbs::TrackT& track);

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
