// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "File.h"
#include "../detail/Decoder.h"
#include <ao/media/mp4/Atom.h>
#include <ao/media/mp4/AtomLayout.h>
#include <ao/utility/ByteView.h>

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ao::tag::mp4
{
  using namespace ao::media::mp4;

  namespace
  {
    using TextSetter =
      library::TrackBuilder::MetadataBuilder& (library::TrackBuilder::MetadataBuilder::*)(std::string_view);
    using NumberSetter =
      library::TrackBuilder::MetadataBuilder& (library::TrackBuilder::MetadataBuilder::*)(std::uint16_t);

    std::span<std::byte const> atomData(AtomView const& view)
    {
      auto const& layout = view.layout<DataAtomLayout>();
      auto const* const data = utility::layout::viewAt<std::byte>(&layout, sizeof(DataAtomLayout));
      auto const size = layout.common.length.value() - sizeof(DataAtomLayout);

      return utility::bytes::view(data, size);
    }

    std::string_view atomTextView(AtomView const& view)
    {
      return utility::bytes::stringView(atomData(view));
    }

    void handleTrackNumbers(library::TrackBuilder& builder, AtomView const& view)
    {
      auto const& layout = view.layout<TrknAtomLayout>();
      builder.metadata().trackNumber(layout.trackNumber.value()).totalTracks(layout.totalTracks.value());
    }

    void handleDiscNumbers(library::TrackBuilder& builder, AtomView const& view)
    {
      auto const& layout = view.layout<DiskAtomLayout>();
      builder.metadata().discNumber(layout.discNumber.value()).totalDiscs(layout.totalDiscs.value());
    }

    template<TextSetter Setter>
    void handleText(library::TrackBuilder& builder, AtomView const& view)
    {
      (builder.metadata().*Setter)(atomTextView(view));
    }

    template<NumberSetter Setter>
    void handleNumber(library::TrackBuilder& builder, AtomView const& view)
    {
      if (auto year = decodeUint16(atomTextView(view)); year)
      {
        (builder.metadata().*Setter)(*year);
      }
    }

    void handleCoverArt(library::TrackBuilder& builder, AtomView const& view)
    {
      builder.metadata().coverArtData(atomData(view));
    }

    using AtomHandler = void (*)(library::TrackBuilder&, AtomView const&);

#include "tag/mp4/AtomDispatch.h"

    static constexpr std::array kIlstPath = {
      std::string_view{"root"},
      std::string_view{"moov"},
      std::string_view{"udta"},
      std::string_view{"meta"},
      std::string_view{"ilst"},
    };

    static constexpr std::array kMdhdPath = {
      std::string_view{"root"},
      std::string_view{"moov"},
      std::string_view{"trak"},
      std::string_view{"mdia"},
      std::string_view{"mdhd"},
    };

    static constexpr std::array kStsdPath = {
      std::string_view{"root"},
      std::string_view{"moov"},
      std::string_view{"trak"},
      std::string_view{"mdia"},
      std::string_view{"minf"},
      std::string_view{"stbl"},
      std::string_view{"stsd"},
    };

    // Helper to extract audio properties from mdhd and stsd
    void extractAudioProperties(library::TrackBuilder& builder, RootAtom const& root, std::size_t fileSize)
    {
      // Get mdhd for sample rate and duration

      if (auto const* const mdhdNode = root.find(kMdhdPath); mdhdNode != nullptr)
      {
        auto const& view = utility::unsafeDowncast<AtomView const>(*mdhdNode);
        auto const& layout = view.layout<MdhdAtomLayout>();
        auto const timescale = layout.timescale.value();
        auto const duration = layout.duration.value();

        if (timescale > 0)
        {
          builder.property().sampleRate(timescale);

          if (duration > 0)
          {
            constexpr std::uint32_t kMsPerSecond = 1000;
            constexpr std::uint32_t kBitsPerByte = 8;
            auto const durationMs =
              static_cast<std::uint32_t>((static_cast<std::uint64_t>(duration) * kMsPerSecond) / timescale);

            if (durationMs > 0)
            {
              builder.property()
                .durationMs(durationMs)
                .bitrate(static_cast<std::uint32_t>((fileSize * kMsPerSecond * kBitsPerByte) / durationMs));
            }
          }
        }
      }

      // Get stsd for channels and bit depth

      if (auto const* const stsdNode = root.find(kStsdPath); stsdNode != nullptr)
      {
        auto const& view = utility::unsafeDowncast<AtomView const>(*stsdNode);

        // stsd contains a version byte (1), flags (3), and then entry count (4)
        // Entries start after 8 bytes of stsd content
        constexpr std::size_t kStsdContentHeaderSize = 8;
        auto const* const stsdBase = utility::layout::asPtr<std::byte>(view.bytes());
        auto const* const data = stsdBase + sizeof(AtomLayout) + kStsdContentHeaderSize;

        // Now data points to the first sample entry (includes length + type)
        auto const& audioLayout =
          *utility::layout::view<AudioSampleEntryLayout>(utility::bytes::view(data, sizeof(AudioSampleEntryLayout)));

        builder.property().channels(audioLayout.channelCount.value()).bitDepth(audioLayout.sampleSize.value());

        // Sample rate is a 16.16 fixed point, extract integer part
        // Only use if non-zero (ALAC may have 0 here, mdhd has correct rate)
        constexpr std::size_t kFixedPointShift = 16;

        if (auto const sampleRateFixed = audioLayout.sampleRate.value(); sampleRateFixed >> kFixedPointShift > 0)
        {
          builder.property().sampleRate(sampleRateFixed >> kFixedPointShift);
        }
      }
    }
  } // namespace

  library::TrackBuilder File::loadTrack() const
  {
    RootAtom root = media::mp4::fromBuffer(utility::bytes::view(_mappedRegion.get_address(), _mappedRegion.get_size()));
    Atom const* const ilstNode = root.find(kIlstPath);

    clearOwnedStrings();
    auto builder = library::TrackBuilder::createNew();

    if (ilstNode != nullptr)
    {
      ilstNode->visitChildren(
        [&](Atom const& atom)
        {
          auto const& view = utility::unsafeDowncast<AtomView const>(atom);
          std::string_view const type = atom.type();

          if (type == "----")
          {
            return true;
          }

          if (auto const* const entry = Mp4AtomDispatchTable::lookupAtomField(type.data(), type.size());
              entry != nullptr)
          {
            entry->handler(builder, view);
            return true;
          }

          builder.custom().add(type, atomTextView(view));
          return true;
        });
    }

    extractAudioProperties(builder, root, _mappedRegion.get_size());

    return builder;
  }
} // namespace ao::tag::mp4
