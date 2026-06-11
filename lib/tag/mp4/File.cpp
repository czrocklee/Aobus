// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "File.h"

#include "../detail/Decoder.h"
#include <ao/library/AudioCodec.h>
#include <ao/library/TrackBuilder.h>
#include <ao/media/mp4/Atom.h>
#include <ao/media/mp4/AtomLayout.h>
#include <ao/tag/TagFile.h>
#include <ao/utility/ByteView.h>

#include <boost/endian/buffers.hpp>

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

    std::uint8_t byteValue(std::byte byte) noexcept
    {
      return static_cast<std::uint8_t>(byte);
    }

    std::uint16_t readBigEndianU16(std::span<std::byte const> bytes, std::size_t offset) noexcept
    {
      return static_cast<std::uint16_t>((static_cast<std::uint16_t>(byteValue(bytes[offset])) << 8U) |
                                        static_cast<std::uint16_t>(byteValue(bytes[offset + 1])));
    }

    struct NumberPair final
    {
      std::uint16_t number = 0;
      std::uint16_t total = 0;
    };

    std::optional<NumberPair> atomNumberPair(AtomView const& view)
    {
      constexpr std::size_t kDataAtomFieldsAfterParentHeader = sizeof(DataAtomLayout) - sizeof(AtomLayout);
      constexpr std::size_t kNumberPairPayloadSize = 6;

      auto const bytes = view.bytes();

      if (bytes.size() < sizeof(DataAtomLayout) + kNumberPairPayloadSize)
      {
        return std::nullopt;
      }

      auto const* const layout = utility::layout::view<DataAtomLayout>(bytes);

      if (auto const dataLength = layout->dataLength.value();
          std::string_view{layout->magic.data(), layout->magic.size()} != "data" ||
          dataLength < kDataAtomFieldsAfterParentHeader + kNumberPairPayloadSize ||
          sizeof(AtomLayout) + dataLength > bytes.size())
      {
        return std::nullopt;
      }

      constexpr std::size_t kPayloadOffset = sizeof(DataAtomLayout);
      constexpr std::size_t kNumberOffset = 2;
      constexpr std::size_t kTotalOffset = 4;

      return NumberPair{.number = readBigEndianU16(bytes, kPayloadOffset + kNumberOffset),
                        .total = readBigEndianU16(bytes, kPayloadOffset + kTotalOffset)};
    }

    void handleTrackNumbers(library::TrackBuilder& builder, AtomView const& view)
    {
      if (auto optPair = atomNumberPair(view); optPair)
      {
        builder.metadata().trackNumber(optPair->number).totalTracks(optPair->total);
      }
    }

    void handleDiscNumbers(library::TrackBuilder& builder, AtomView const& view)
    {
      if (auto optPair = atomNumberPair(view); optPair)
      {
        builder.metadata().discNumber(optPair->number).totalDiscs(optPair->total);
      }
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

    AudioSampleEntryLayout const* firstAudioSampleEntry(AtomView const& stsdView)
    {
      auto const bytes = stsdView.bytes();

      if (bytes.size() < sizeof(StsdAtomLayout) + sizeof(AudioSampleEntryLayout))
      {
        return nullptr;
      }

      if (auto const& stsdLayout = stsdView.layout<StsdAtomLayout>(); stsdLayout.entryCount.value() == 0)
      {
        return nullptr;
      }

      auto const entryBytes = bytes.subspan(sizeof(StsdAtomLayout));

      if (entryBytes.size() < sizeof(AudioSampleEntryLayout))
      {
        return nullptr;
      }

      auto const* const entryLayout = utility::layout::view<AtomLayout>(entryBytes);

      if (entryLayout->length.value() < sizeof(AudioSampleEntryLayout) ||
          entryLayout->length.value() > entryBytes.size())
      {
        return nullptr;
      }

      return utility::layout::view<AudioSampleEntryLayout>(entryBytes);
    }

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
        auto const* const audioLayout = firstAudioSampleEntry(view);

        if (audioLayout == nullptr)
        {
          return;
        }

        auto const sampleEntryType = std::string_view{audioLayout->common.type.data(), audioLayout->common.type.size()};
        auto codec = library::AudioCodec::Unknown;

        if (sampleEntryType == "alac")
        {
          codec = library::AudioCodec::Alac;
        }
        else if (sampleEntryType == "mp4a")
        {
          codec = library::AudioCodec::Aac;
        }

        builder.property()
          .channels(static_cast<std::uint8_t>(audioLayout->channelCount.value()))
          .bitDepth(static_cast<std::uint8_t>(audioLayout->sampleSize.value()))
          .codec(codec);

        // Sample rate is a 16.16 fixed point, extract integer part
        // Only use if non-zero (ALAC may have 0 here, mdhd has correct rate)
        constexpr std::size_t kFixedPointShift = 16;

        if (auto const sampleRateFixed = audioLayout->sampleRate.value(); sampleRateFixed >> kFixedPointShift > 0)
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
