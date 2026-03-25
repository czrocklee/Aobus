// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "../Decoder.h"
#include "MetadataBlock.h"
#include <rs/Exception.h>
#include <rs/tag/flac/File.h>
#include <rs/utility/ByteView.h>

#include <boost/algorithm/string/compare.hpp>

#include <algorithm>
#include <ranges>
#include <span>
#include <functional>
#include <iostream>
#include <map>
#include <string_view>

namespace rs::tag::flac
{
  namespace
  {
    template<MetaField Field, typename Decoder>
    struct FieldSetter
    {
      void operator()(Metadata& meta, std::span<std::byte const> buffer)
      {
        meta.set(Field, Decoder::decode(buffer.data(), buffer.size()));
      }
    };

    template<MetaField PrimaryField, MetaField SecondaryField, typename Decoder>
    struct SlashFieldsSetter
    {
      void operator()(Metadata& meta, std::span<std::byte const> buffer)
      {
        auto str = utility::asString(buffer);

        if (auto const& iter = std::ranges::find(str, '/'); iter != str.end())
        {
          auto dist = std::distance(str.begin(), iter);
          meta.set(PrimaryField, Decoder::decode(str.data(), dist));
          meta.set(SecondaryField, Decoder::decode(iter, std::distance(iter, str.end()) - 1));
        }
        else
        {
          meta.set(PrimaryField, Decoder::decode(str.data(), str.size()));
        }
      }
    };

    struct CaseInsensitiveComparator
    {
      using is_transparent = void;

      bool operator()(std::string_view const& lhs, std::string_view const& rhs) const
      {
        return std::lexicographical_compare(lhs.begin(), lhs.end(), rhs.begin(), rhs.end(), boost::is_iless{});
      }
    };

    std::map<std::string, std::function<void(Metadata&, std::span<std::byte const>)>, CaseInsensitiveComparator>
      const MetadataSetters = {
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

  Metadata File::loadMetadata() const
  {
    if (_mappedRegion.get_size() < 4 || std::memcmp(_mappedRegion.get_address(), "fLaC", 4) != 0)
    {
      RS_THROW(rs::Exception, "unrecognized flac file content");
    }

    Metadata metadata;
    MetadataBlockViewIterator iter{
      static_cast<char const*>(_mappedRegion.get_address()) + 4, _mappedRegion.get_size() - 4};
    MetadataBlockViewIterator end{};

    for (; iter != end; ++iter)
    {
      switch (iter->type())
      {
        case MetadataBlockType::VorbisComment:
        {
          for (VorbisCommentBlockView block{iter->data()}; auto metaLine : block.comments())
          {
            if (auto pos = metaLine.find('='); pos != std::string_view::npos)
            {
              std::string_view key = metaLine.substr(0, pos);
              std::string_view value = metaLine.substr(pos + 1);

              if (auto iter = MetadataSetters.find(key); iter != MetadataSetters.end())
              {
                std::invoke(iter->second, metadata, utility::asBytes(value));
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
          auto block = PictureBlockView{iter->data()};
          auto blob = block.blob();
          metadata.set(MetaField::AlbumArt, BlobDecoder::decode(blob.data(), blob.size()));
          break;
        }

        default:
          break;
      }
    }

    return metadata;
  }

  void File::saveMetadata([[maybe_unused]] Metadata const& metadata) {}
}