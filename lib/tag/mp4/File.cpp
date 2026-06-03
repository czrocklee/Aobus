// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "File.h"

#include "../detail/Decoder.h"
#include <ao/library/TrackBuilder.h>
#include <ao/media/mp4/Atom.h>
#include <ao/media/mp4/AtomLayout.h>
#include <ao/tag/TagFile.h>
#include <ao/utility/ByteView.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

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
      if (auto optYear = decodeUint16(atomTextView(view)); optYear)
      {
        (builder.metadata().*Setter)(*optYear);
      }
    }

    void handleCoverArt(library::TrackBuilder& builder, AtomView const& view)
    {
      builder.metadata().coverArtData(atomData(view));
    }

    std::string atomTypeToUtf8(std::string_view const type)
    {
      if (type.size() != 4)
      {
        return std::string{type};
      }

      // Apple-specific tags often use 0xA9 (Copyright) as a prefix.
      // In UTF-8, Copyright is 0xC2 0xA9.
      constexpr auto kCopyrightPrefix = static_cast<unsigned char>(0xA9);

      if (static_cast<unsigned char>(type[0]) == kCopyrightPrefix)
      {
        auto result = std::string{"©"};
        result += type.substr(1);
        return result;
      }

      return std::string{type};
    }

    std::optional<std::string> atomValueToString(AtomView const& view)
    {
      try
      {
        auto const& layout = view.layout<DataAtomLayout>();
        auto const type = layout.type.value();
        auto const data = atomData(view);

        if (type == 1) // UTF-8
        {
          return std::string{utility::bytes::stringView(data)};
        }

        constexpr auto kTypeSignedInteger = 21;
        constexpr auto kTypeUnsignedInteger = 22;

        if (type == kTypeSignedInteger || type == kTypeUnsignedInteger)
        {
          if (data.size() == 1)
          {
            return std::to_string(static_cast<std::uint8_t>(data[0]));
          }

          if (data.size() == 2)
          {
            return std::to_string(utility::layout::view<boost::endian::big_uint16_buf_t>(data)->value());
          }

          if (data.size() == 4)
          {
            return std::to_string(utility::layout::view<boost::endian::big_uint32_buf_t>(data)->value());
          }
        }
      }
      catch (...)
      {
        return std::nullopt;
      }

      return std::nullopt;
    }

    void handleFreeform(File const& file, library::TrackBuilder& builder, AtomView const& view)
    {
      auto nameStr = std::string{};
      auto optValueStr = std::optional<std::string>{};

      view.visitChildren(
        [&](Atom const& child)
        {
          auto const& childView = utility::unsafeDowncast<AtomView const>(child);

          if (auto const type = childView.type(); type == "name")
          {
            nameStr = std::string{utility::bytes::stringView(childView.bytes().subspan(8))};
          }
          else if (type == "data")
          {
            optValueStr = atomValueToString(childView);
          }

          return true;
        });

      if (!nameStr.empty() && optValueStr)
      {
        builder.custom().add(
          detail::stashOwnedString(file, std::move(nameStr)), detail::stashOwnedString(file, std::move(*optValueStr)));
      }
    }

    using AtomHandler = void (*)(library::TrackBuilder&, AtomView const&);

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#include "tag/mp4/AtomDispatch.h"
#pragma GCC diagnostic pop

    constexpr std::array kIlstPath = {
      std::string_view{"root"},
      std::string_view{"moov"},
      std::string_view{"udta"},
      std::string_view{"meta"},
      std::string_view{"ilst"},
    };

    constexpr std::array kMdhdPath = {
      std::string_view{"root"},
      std::string_view{"moov"},
      std::string_view{"trak"},
      std::string_view{"mdia"},
      std::string_view{"mdhd"},
    };

    constexpr std::array kStsdPath = {
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

        if (auto const timescale = layout.timescale.value(); timescale > 0)
        {
          builder.property().sampleRate(timescale);

          if (auto const duration = layout.duration.value(); duration > 0)
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

        builder.property()
          .channels(static_cast<std::uint8_t>(audioLayout.channelCount.value()))
          .bitDepth(static_cast<std::uint8_t>(audioLayout.sampleSize.value()));

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
    RootAtom const root = media::mp4::fromBuffer(utility::bytes::view(address(), size()));
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
            handleFreeform(*this, builder, view);
            return true;
          }

          if (auto const* const entry = Mp4AtomDispatchTable::lookupAtomField(type.data(), type.size());
              entry != nullptr)
          {
            entry->handler(builder, view);
            return true;
          }

          if (auto optValue = atomValueToString(view); optValue)
          {
            builder.custom().add(detail::stashOwnedString(*this, atomTypeToUtf8(type)),
                                 detail::stashOwnedString(*this, std::move(*optValue)));
          }

          return true;
        });
    }

    extractAudioProperties(builder, root, size());

    return builder;
  }
} // namespace ao::tag::mp4
