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

#include "../Decoder.h"
#include "MetadataBlock.h"
#include <rs/tag/flac/File.h>
#include <rs/common/Exception.h>

#include <boost/asio/buffer.hpp>
#include <boost/algorithm/string/compare.hpp>
#include <string_view>
#include <algorithm>
#include <map>
#include <iostream>

namespace rs::tag::flac
{
  namespace
  {
    template<MetaField Field, typename Decoder>
    struct FieldSetter
    {
      void operator()(Metadata& meta, boost::asio::const_buffer buffer)
      {
        meta.set(Field, Decoder::decode(buffer.data(), buffer.size()));
      }
    };

    template<MetaField PrimaryField, MetaField SecondaryField, typename Decoder>
    struct SlashFieldsSetter
    {
      void operator()(Metadata& meta, boost::asio::const_buffer buffer)
      {
        const char* begin = static_cast<const char*>(buffer.data());
        const char* end = begin + buffer.size();

        if (auto iter = std::find(begin, end, '/'); iter != end)
        {
          meta.set(PrimaryField, Decoder::decode(begin, iter - begin));
          meta.set(SecondaryField, Decoder::decode(iter, end - iter - 1));
        }
        else
        {
          meta.set(PrimaryField, Decoder::decode(begin, buffer.size()));
        }
      }
    };

    struct CaseInsensitiveComparator
    {
      using is_transparent = void;

      bool operator()(const std::string_view& lhs, const std::string_view& rhs) const
      {
        return std::lexicographical_compare(lhs.begin(), lhs.end(), rhs.begin(), rhs.end(), boost::is_iless{});
      }
    };

    std::map<std::string, std::function<void(Metadata&, boost::asio::const_buffer)>, CaseInsensitiveComparator>
      MetadataSetters = {
        {"TITLE", FieldSetter<MetaField::Title, StringDecoder>{}},
        {"ARTIST", FieldSetter<MetaField::Artist, StringDecoder>{}},
        {"ALBUM", FieldSetter<MetaField::Album, StringDecoder>{}},
        {"ALBUMARTIST", FieldSetter<MetaField::AlbumArtist, StringDecoder>{}},
        {"TRACKNUMBER", SlashFieldsSetter<MetaField::TrackNumber, MetaField::TotalTracks, IntDecoder>{}},
        {"TRACKTOTAL", FieldSetter<MetaField::TotalTracks, IntDecoder>{}},
        {"TOTALTRACKS", FieldSetter<MetaField::TotalTracks, IntDecoder>{}},
        {"DISCNUMBER", SlashFieldsSetter<MetaField::DiscNumber, MetaField::TotalDiscs, IntDecoder>{}},
        {"DISCTOTAL", FieldSetter<MetaField::TotalDiscs, IntDecoder>{}},
        {"TOTALDISCS", FieldSetter<MetaField::TotalDiscs, IntDecoder>{}},
        {"GENRE", FieldSetter<MetaField::Genre, StringDecoder>{}}};
  }

  const Metadata File::loadMetadata() const
  {
    if (_mappedRegion.get_size() < 4 || std::memcmp(_mappedRegion.get_address(), "fLaC", 4) != 0)
    {
      RS_THROW(common::Exception, "unrecognized flac file content");
    }

    Metadata metadata;
    MetadataBlockViewIterator iter{static_cast<const char*>(_mappedRegion.get_address()) + 4, _mappedRegion.get_size() - 4};
    MetadataBlockViewIterator end{};

    for (; iter != end; ++iter)
    {
      // std::cout << " type " << static_cast<int>(iter->type()) << " size " << iter->size() << std::endl;
      switch (iter->type())
      {
        case MetadataBlockType::VorbisComment:
        {
          VorbisCommentBlockView block{iter->data()};

          for (auto metaLine : block.comments())
          {
            if (auto pos = metaLine.find('='); pos != std::string_view::npos)
            {
              std::string_view key = metaLine.substr(0, pos);
              std::string_view value = metaLine.substr(pos + 1);

              if (auto iter = MetadataSetters.find(key); iter != MetadataSetters.end())
              {
                std::invoke(iter->second, metadata, boost::asio::buffer(value.data(), value.size()));
              }
              else
              {
                metadata.setCustom(key, std::string{value});
              }
            }
          }

          break;
        }

        case MetadataBlockType::Picture:
        {
          PictureBlockView block{iter->data()};
          auto blob = block.blob();
          metadata.set(MetaField::AlbumArt, BlobDecoder::decode(blob.data(), blob.size()));
          break;
        }

        default: break;
      }
    }

    return metadata;
  }

  void File::saveMetadata(const Metadata& metadata) {}
}