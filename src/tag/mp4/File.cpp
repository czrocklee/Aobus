// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "../Decoder.h"
#include "Atom.h"
#include <rs/tag/mp4/File.h>

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rs::tag::mp4
{
  namespace
  {
    using TextSetter = rs::core::TrackBuilder::MetadataBuilder& (rs::core::TrackBuilder::MetadataBuilder::*)(std::string_view);
    using NumberSetter = rs::core::TrackBuilder::MetadataBuilder& (rs::core::TrackBuilder::MetadataBuilder::*)(std::uint16_t);

    std::span<std::byte const> atomData(AtomView const& view)
    {
      auto const& layout = view.layout<DataAtomLayout>();
      auto const* data = reinterpret_cast<std::byte const*>(&layout + 1);
      auto const size = layout.common.length.value() - sizeof(DataAtomLayout);
      return {data, size};
    }

    std::string_view atomTextView(AtomView const& view)
    {
      auto data = atomData(view);
      return {reinterpret_cast<char const*>(data.data()), data.size()};
    }

    void handleTrackNumbers(rs::core::TrackBuilder& builder, AtomView const& view)
    {
      auto const& layout = view.layout<TrknAtomLayout>();
      builder.metadata().trackNumber(layout.trackNumber.value()).totalTracks(layout.totalTracks.value());
    }

    void handleDiscNumbers(rs::core::TrackBuilder& builder, AtomView const& view)
    {
      auto const& layout = view.layout<DiskAtomLayout>();
      builder.metadata().discNumber(layout.discNumber.value()).totalDiscs(layout.totalDiscs.value());
    }

    template<TextSetter Setter>
    void handleText(rs::core::TrackBuilder& builder, AtomView const& view)
    {
      (builder.metadata().*Setter)(atomTextView(view));
    }

    template<NumberSetter Setter>
    void handleNumber(rs::core::TrackBuilder& builder, AtomView const& view)
    {
      if (auto year = decodeUint16(atomData(view)); year) { (builder.metadata().*Setter)(*year); }
    }

    void handleCoverArt(rs::core::TrackBuilder& builder, AtomView const& view)
    {
      builder.metadata().coverArtData(atomData(view));
    }

    using AtomHandler = void (*)(rs::core::TrackBuilder&, AtomView const&);

#include "tag/mp4/AtomDispatch.h"

    constexpr std::array kIlstPath = {
      std::string_view{"root"},
      std::string_view{"moov"},
      std::string_view{"udta"},
      std::string_view{"meta"},
      std::string_view{"ilst"},
    };

    // Path to mdhd: moov > trak > mdia > mdhd
    constexpr std::array kMdhdPath = {
      std::string_view{"root"},
      std::string_view{"moov"},
      std::string_view{"trak"},
      std::string_view{"mdia"},
      std::string_view{"mdhd"},
    };

    // Path to stsd: moov > trak > mdia > minf > stbl > stsd
    constexpr std::array kStsdPath = {
      std::string_view{"root"},
      std::string_view{"moov"},
      std::string_view{"trak"},
      std::string_view{"mdia"},
      std::string_view{"minf"},
      std::string_view{"stbl"},
      std::string_view{"stsd"},
    };

    template<std::size_t Extent>
    Atom const* findNode(Atom const& node, std::span<std::string_view const, Extent> path, std::size_t startPos)
    {
      if (startPos >= path.size() || path[startPos] != node.type()) { return nullptr; }

      if (startPos == path.size() - 1) { return &node; }

      Atom const* found = nullptr;
      node.visitChildren([&](auto const& child) { return ((found = findNode(child, path, startPos + 1)) == nullptr); });
      return found;
    }

    Atom const* findIlstNode(RootAtom const& root)
    {
      return findNode(root, std::span<std::string_view const, kIlstPath.size()>{kIlstPath}, 0);
    }

    Atom const* findMdhdNode(RootAtom const& root)
    {
      return findNode(root, std::span<std::string_view const, kMdhdPath.size()>{kMdhdPath}, 0);
    }

    Atom const* findStsdNode(RootAtom const& root)
    {
      return findNode(root, std::span<std::string_view const, kStsdPath.size()>{kStsdPath}, 0);
    }

    // Helper to extract audio properties from mdhd and stsd
    void extractAudioProperties(rs::core::TrackBuilder& builder, RootAtom const& root, std::size_t fileSize)
    {
      // Get mdhd for sample rate and duration
      if (auto const* mdhdNode = findMdhdNode(root); mdhdNode != nullptr)
      {
        auto const& view = static_cast<AtomView const&>(*mdhdNode);
        auto const& layout = view.layout<MdhdAtomLayout>();
        auto const timescale = layout.timescale.value();
        auto const duration = layout.duration.value();

        if (timescale > 0)
        {
          builder.property().sampleRate(timescale);

          if (duration > 0)
          {
            auto const durationMs = static_cast<std::uint32_t>((static_cast<std::uint64_t>(duration) * 1000) / timescale);
            if (durationMs > 0)
            {
              builder.property().durationMs(durationMs).bitrate(
                static_cast<std::uint32_t>((fileSize * 8000) / durationMs));
            }
          }
        }
      }

      // Get stsd for channels and bit depth
      if (auto const* stsdNode = findStsdNode(root); stsdNode != nullptr)
      {
        auto const& view = static_cast<AtomView const&>(*stsdNode);
        auto const& stsdLayout = view.layout<AtomLayout>();

        // stsd contains a version byte (1), flags (3), and then entry count (4)
        // Entries start after 8 bytes of stsd content
        auto const* data = reinterpret_cast<std::uint8_t const*>(&stsdLayout) + sizeof(AtomLayout) + 8;

        // Now data points to the first sample entry (includes length + type)
        auto const& audioLayout = *reinterpret_cast<AudioSampleEntryLayout const*>(data);
        builder.property().channels(audioLayout.channelCount.value()).bitDepth(audioLayout.sampleSize.value());

        // Sample rate is a 16.16 fixed point, extract integer part
        // Only use if non-zero (ALAC may have 0 here, mdhd has correct rate)
        auto const sampleRateFixed = audioLayout.sampleRate.value();
        if (sampleRateFixed >> 16 > 0)
        {
          builder.property().sampleRate(sampleRateFixed >> 16);
        }
      }
    }
  } // namespace

  rs::core::TrackBuilder File::loadTrack() const
  {
    RootAtom root = rs::tag::mp4::fromBuffer(_mappedRegion.get_address(), _mappedRegion.get_size());
    Atom const* ilstNode = findIlstNode(root);

    clearOwnedStrings();
    auto builder = rs::core::TrackBuilder::createNew();

    if (ilstNode != nullptr)
    {
      ilstNode->visitChildren([&](Atom const& atom) {
        auto const& view = static_cast<AtomView const&>(atom);
        std::string_view type = atom.type();

        if (auto const* entry = Mp4AtomDispatchTable::lookupAtomField(type.data(), type.size()); entry != nullptr)
        {
          entry->handler(builder, view);
          return true;
        }

        builder.custom().add(type, atomTextView(view));
        return true;
      });
    }

    // Extract audio properties from mdhd and stsd
    extractAudioProperties(builder, root, _mappedRegion.get_size());

    return builder;
  }
} // namespace rs::tag::mp4
