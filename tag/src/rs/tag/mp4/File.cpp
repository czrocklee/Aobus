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

#include <rs/tag/mp4/File.h>
#include <boost/asio/buffer.hpp>
#include <boost/endian/conversion.hpp>
#include "Atom.h"
#include <iostream>
#include <map>
#include <charconv>

namespace rs::tag::mp4
{
  namespace
  {
    const Atom* findNode(const Atom& node, const std::vector<std::string>& path, std::size_t startPos)
    {
      if (startPos >= path.size() || path[startPos] != node.type())
      {
        return nullptr;
      }

      if (startPos == path.size() - 1)
      {
        return &node;
      }

      const Atom* found = nullptr;
      node.visitChildren([&](const auto& child) { return ((found = findNode(child, path, startPos + 1)) == nullptr); });
      return found;
    }

    struct StringDecoder
    {
      static ValueType decode(const DataAtomLayout& layout)
      {
        const char* data = reinterpret_cast<const char*>(&layout + 1);
        std::size_t size = layout.common.length.value() - sizeof(DataAtomLayout);
        return std::string{data, size};
      }
    };

    struct IntDecoder
    {
      static ValueType decode(const DataAtomLayout& layout)
      {
        const char* data = reinterpret_cast<const char*>(&layout + 1);
        std::size_t size = layout.common.length.value() - sizeof(DataAtomLayout);
        std::int64_t result;
        auto [_, ec] = std::from_chars(data, data + size, result);
        return ec == std::errc() ? ValueType{result} : ValueType{};
      }
    };

    struct BlobDecoder
    {
      static ValueType decode(const DataAtomLayout& layout)
      {
        const char* data = reinterpret_cast<const char*>(&layout + 1);
        std::size_t size = layout.common.length.value() - sizeof(DataAtomLayout);
        std::vector<char> blob;
        blob.assign(data, data + size);
        return {std::move(blob)};
      }
    };

    template<MetaField Field, typename Decoder>
    struct FieldSetter
    {
      void operator()(Metadata& meta, const Atom& atom)
      {
        const auto& layout = static_cast<const AtomView&>(atom).layout<DataAtomLayout>();
        meta.set(Field, Decoder::decode(layout));
      }
    };

    std::map<std::string, std::function<void(Metadata&, const Atom&)>, std::less<>> MetadataSetters = {
      {TrknAtomLayout::Type,
       [](auto& meta, const auto& atom) {
         const auto& trkn = static_cast<const AtomView&>(atom).layout<TrknAtomLayout>();
         meta.set(MetaField::TrackNumber, static_cast<std::uint64_t>(trkn.trackNumber.value()));
         meta.set(MetaField::TotalTracks, static_cast<std::uint64_t>(trkn.totalTracks.value()));
       }},
      {DiskAtomLayout::Type,
       [](auto& meta, const auto& atom) {
         const auto& disk = static_cast<const AtomView&>(atom).layout<DiskAtomLayout>();
         meta.set(MetaField::DiscNumber, static_cast<std::uint64_t>(disk.discNumber.value()));
         meta.set(MetaField::TotalDiscs, static_cast<std::uint64_t>(disk.totalDiscs.value()));
       }},

      {"\251nam", FieldSetter<MetaField::Title, StringDecoder>{}},
      {"\251ART", FieldSetter<MetaField::Artist, StringDecoder>{}},
      {"\251alb", FieldSetter<MetaField::Album, StringDecoder>{}},
      {"aART", FieldSetter<MetaField::AlbumArtist, StringDecoder>{}},
      {"covr", FieldSetter<MetaField::AlbumArt, BlobDecoder>{}},
      {"grne", FieldSetter<MetaField::Genre, StringDecoder>{}},
      {"\251day", FieldSetter<MetaField::Year, IntDecoder>{}}
    };
  }

  const Metadata File::loadMetadata() const
  {
    RootAtom root = rs::tag::mp4::fromBuffer(_mappedRegion.get_address(), _mappedRegion.get_size());
    const Atom* ilstNode = findNode(root, {"root", "moov", "udta", "meta", "ilst"}, 0);
    Metadata metadata;
    ilstNode->visitChildren([&metadata](const Atom& atom) {
      if (auto iter = MetadataSetters.find(atom.type()); iter != MetadataSetters.end())
      {
        std::invoke(iter->second, metadata, atom);
      }
      else
      {
        const auto& data = static_cast<const AtomView&>(atom).layout<DataAtomLayout>();
        std::string value{reinterpret_cast<const char*>(&data + 1), data.common.length.value() - sizeof(DataAtomLayout)};
        //std::cout << atom.type() <<  "  xxx  " << value << value.size() << std::endl;
        metadata.setCustom(atom.type(), std::move(value));
      }
      
      return true;
    });

    return metadata;
  }

  void File::saveMetadata(const Metadata& metadata) {}
}