/*
 * Copyright (C) <year> <name of author>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of  MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <rs/ml/file/MusicFile.h>
#include <taglib/taglib.h>
#include <taglib/flacfile.h>
#include <taglib/tpropertymap.h>
#include <taglib/mp4tag.h>

namespace
{
  std::string loadProperty(const TagLib::PropertyMap& map, const char* key)
  {
    auto iter = map.find(key);
    return iter != map.end() && !iter->second.isEmpty() ? iter->second.front().toCString(true) : "";
  }
}

namespace rs::ml::file
{
  fbs::TrackT MusicFile::loadTrack() const
  {
    fbs::TrackT track;
    const auto* tag = _fileRef.tag();

    {
      track.meta = std::make_unique<fbs::MetadataT>();
      track.meta->title = tag->title().toCString(true);
      track.meta->album = tag->album().toCString(true);
      track.meta->albumArtist = loadProperty(tag->properties(), "ALBUMARTIST");
      track.meta->artist = tag->artist().toCString(true);
      track.meta->genre = tag->genre().toCString(true);
      track.meta->year = tag->year();
      track.meta->trackNumber = tag->track();
    }

    return track;
  }

  TagLib::ByteVector MusicFile::loadAlbumArt() const
  {
    if (auto* tag = dynamic_cast<TagLib::MP4::Tag*>(_fileRef.tag()))
    {
      TagLib::MP4::ItemListMap itemsListMap = tag->itemListMap();
      TagLib::MP4::Item coverItem = itemsListMap["covr"];
      TagLib::MP4::CoverArtList coverArtList = coverItem.toCoverArtList();

      if (!coverArtList.isEmpty())
      {
        TagLib::MP4::CoverArt coverArt = coverArtList.front();
        return coverArt.data();
      }
    }

    return {};
  }
}
