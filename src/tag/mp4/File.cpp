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
#include "Atom.h"
#include <charconv>
#include <map>
#include <rs/tag/mp4/File.h>

namespace rs::tag::mp4
{
  namespace
  {
    Atom const* findNode(Atom const& node, std::vector<std::string> const& path, std::size_t startPos)
    {
      if (startPos >= path.size() || path[startPos] != node.type())
      {
        return nullptr;
      }

      if (startPos == path.size() - 1)
      {
        return &node;
      }

      Atom const* found = nullptr;
      node.visitChildren([&](auto const& child) { return ((found = findNode(child, path, startPos + 1)) == nullptr); });
      return found;
    }

    template<MetaField Field, typename Decoder>
    struct FieldSetter
    {
      void operator()(Metadata& meta, Atom const& atom)
      {
        auto const& layout = static_cast<AtomView const&>(atom).layout<DataAtomLayout>();
        auto const* buffer = reinterpret_cast<char const*>(&layout + 1);
        std::size_t size = layout.common.length.value() - sizeof(DataAtomLayout);
        meta.set(Field, Decoder::decode(buffer, size));
      }
    };

    std::map<std::string, std::function<void(Metadata&, Atom const&)>, std::less<>> MetadataSetters = {
      {TrknAtomLayout::Type,
       [](auto& meta, auto const& atom) {
         auto const& trkn = static_cast<AtomView const&>(atom).layout<TrknAtomLayout>();
         meta.set(MetaField::TrackNumber, static_cast<std::int64_t>(trkn.trackNumber.value()));
         meta.set(MetaField::TotalTracks, static_cast<std::int64_t>(trkn.totalTracks.value()));
       }},
      {DiskAtomLayout::Type,
       [](auto& meta, auto const& atom) {
         auto const& disk = static_cast<AtomView const&>(atom).layout<DiskAtomLayout>();
         meta.set(MetaField::DiscNumber, static_cast<std::int64_t>(disk.discNumber.value()));
         meta.set(MetaField::TotalDiscs, static_cast<std::int64_t>(disk.totalDiscs.value()));
       }},

      {"\251nam", FieldSetter<MetaField::Title, StringDecoder>{}},
      {"\251ART", FieldSetter<MetaField::Artist, StringDecoder>{}},
      {"\251alb", FieldSetter<MetaField::Album, StringDecoder>{}},
      {"aART", FieldSetter<MetaField::AlbumArtist, StringDecoder>{}},
      {"covr", FieldSetter<MetaField::AlbumArt, BlobDecoder>{}},
      {"grne", FieldSetter<MetaField::Genre, StringDecoder>{}},
      {"\251day", FieldSetter<MetaField::Year, IntDecoder>{}}};
  }

  Metadata File::loadMetadata() const
  {
    RootAtom root = rs::tag::mp4::fromBuffer(_mappedRegion.get_address(), _mappedRegion.get_size());
    Atom const* ilstNode = findNode(root, {"root", "moov", "udta", "meta", "ilst"}, 0);
    Metadata metadata;
    ilstNode->visitChildren([&metadata](Atom const& atom) {
      if (auto iter = MetadataSetters.find(atom.type()); iter != MetadataSetters.end())
      {
        std::invoke(iter->second, metadata, atom);
      }
      else
      {
        auto const& data = static_cast<AtomView const&>(atom).layout<DataAtomLayout>();
        std::string value{
          reinterpret_cast<char const*>(&data + 1), data.common.length.value() - sizeof(DataAtomLayout)};
        metadata.setCustom(atom.type(), std::move(value));
      }

      return true;
    });

    return metadata;
  }

  void File::saveMetadata(Metadata const&) {}
}