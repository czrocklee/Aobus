// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "../Decoder.h"
#include "Atom.h"
#include <rs/tag/mp4/File.h>

#include <charconv>
#include <map>

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