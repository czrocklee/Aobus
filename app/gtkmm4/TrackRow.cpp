#include "TrackRow.h"

#include <cstddef>
#include <cstdint>
#include <string>

TrackRow::TrackRow()
{
  _trackId = TrackId(0);
  _resourceId = 0;
}

Glib::RefPtr<TrackRow> TrackRow::create(TrackId id, const rs::fbs::TrackT& track)
{
  std::string artistStr, albumStr, titleStr;

  if (track.meta)
  {
    if (!track.meta->artist.empty())
    {
      artistStr = track.meta->artist;
    }

    if (!track.meta->album.empty())
    {
      albumStr = track.meta->album;
    }

    if (!track.meta->title.empty())
    {
      titleStr = track.meta->title;
    }
  }

  std::string tagsStr;

  for (std::size_t i = 0; i < track.tags.size(); ++i)
  {
    if (i > 0)
    {
      tagsStr += ", ";
    }

    tagsStr += track.tags[i];
  }

  std::uint64_t resourceId = 0;

  if (!track.rsrc.empty())
  {
    resourceId = track.rsrc.front()->id;
  }

  auto obj = Glib::make_refptr_for_instance<TrackRow>(new TrackRow());
  obj->_trackId = id;
  obj->_artist = Glib::ustring(artistStr);
  obj->_album = Glib::ustring(albumStr);
  obj->_title = Glib::ustring(titleStr);
  obj->_tags = Glib::ustring(tagsStr);
  obj->_resourceId = resourceId;

  return obj;
}
