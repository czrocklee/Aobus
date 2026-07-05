// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "File.h"

#include "../detail/Decoder.h"
#include <ao/AudioCodec.h>
#include <ao/Error.h>
#include <ao/library/CoverArt.h>
#include <ao/library/TrackBuilder.h>
#include <ao/media/mp4/Atom.h>
#include <ao/media/mp4/AtomLayout.h>
#include <ao/media/mp4/TrackSelection.h>
#include <ao/tag/TagFile.h>
#include <ao/tag/detail/TagError.h>
#include <ao/utility/ByteView.h>

#include <array>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <span>
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

    // Safely overlay the data-atom header onto an atom's bytes. Returns nullptr when
    // the atom is shorter than the fixed header. Always-checked (release builds strip
    // the gsl contracts inside layout<>()).
    DataAtomLayout const* dataAtomLayout(AtomView const& view) noexcept
    {
      return utility::bytes::tryLayout<DataAtomLayout>(view.bytes());
    }

    std::span<std::byte const> atomData(AtomView const& view)
    {
      // The atom's byte span is sized to its declared length, so the payload is
      // everything past the fixed data-atom header. Guard against a header that does
      // not fit to avoid an unsigned underflow on the payload size.
      if (dataAtomLayout(view) == nullptr)
      {
        return {};
      }

      return view.bytes().subspan(sizeof(DataAtomLayout));
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

    std::uint32_t readBigEndianU32(std::span<std::byte const> bytes, std::size_t offset) noexcept
    {
      return (static_cast<std::uint32_t>(byteValue(bytes[offset])) << 24U) |
             (static_cast<std::uint32_t>(byteValue(bytes[offset + 1])) << 16U) |
             (static_cast<std::uint32_t>(byteValue(bytes[offset + 2])) << 8U) |
             static_cast<std::uint32_t>(byteValue(bytes[offset + 3]));
    }

    bool equalsAsciiCaseInsensitive(std::string_view lhs, std::string_view rhs) noexcept
    {
      if (lhs.size() != rhs.size())
      {
        return false;
      }

      for (std::size_t index = 0; index < lhs.size(); ++index)
      {
        if (std::tolower(static_cast<unsigned char>(lhs[index])) !=
            std::tolower(static_cast<unsigned char>(rhs[index])))
        {
          return false;
        }
      }

      return true;
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
        builder.metadata().trackNumber(optPair->number).trackTotal(optPair->total);
      }
    }

    void handleDiscNumbers(library::TrackBuilder& builder, AtomView const& view)
    {
      if (auto optPair = atomNumberPair(view); optPair)
      {
        builder.metadata().discNumber(optPair->number).discTotal(optPair->total);
      }
    }

    template<TextSetter Setter>
    void handleText(library::TrackBuilder& builder, AtomView const& view)
    {
      (builder.metadata().*Setter)(atomTextView(view));
    }

    [[maybe_unused]] std::string_view atomPayloadAfter(std::span<std::byte const> bytes, std::size_t offset)
    {
      if (bytes.size() <= offset)
      {
        return {};
      }

      return utility::bytes::stringView(bytes.subspan(offset));
    }

    [[maybe_unused]] void handleFreeform(library::TrackBuilder& builder, AtomView const& view)
    {
      auto remaining = view.bytes();

      if (remaining.size() <= sizeof(AtomLayout))
      {
        return;
      }

      remaining = remaining.subspan(sizeof(AtomLayout));

      auto mean = std::string_view{};
      auto name = std::string_view{};
      auto value = std::string_view{};

      while (remaining.size() >= sizeof(AtomLayout))
      {
        auto const* const layout = utility::layout::view<AtomLayout>(remaining);
        auto const length = static_cast<std::size_t>(layout->length.value());

        if (length < sizeof(AtomLayout) || length > remaining.size())
        {
          break;
        }

        auto const childBytes = remaining.subspan(0, length);
        auto const type = utility::bytes::stringView(utility::bytes::view(layout->type));

        if (type == "mean")
        {
          mean = atomPayloadAfter(childBytes, sizeof(AtomLayout) + sizeof(std::uint32_t));
        }
        else if (type == "name")
        {
          name = atomPayloadAfter(childBytes, sizeof(AtomLayout) + sizeof(std::uint32_t));
        }
        else if (type == "data")
        {
          value = atomPayloadAfter(childBytes, sizeof(DataAtomLayout));
        }

        remaining = remaining.subspan(length);
      }

      if (mean != "com.apple.iTunes" || name.empty())
      {
        return;
      }

      if (equalsAsciiCaseInsensitive(name, "conductor"))
      {
        builder.metadata().conductor(value);
      }
      else if (equalsAsciiCaseInsensitive(name, "ensemble") ||
               (equalsAsciiCaseInsensitive(name, "orchestra") && builder.metadata().ensemble().empty()))
      {
        builder.metadata().ensemble(value);
      }
      else if (equalsAsciiCaseInsensitive(name, "soloist"))
      {
        builder.metadata().soloist(value);
      }
    }

    template<NumberSetter Setter>
    void handleNumber(library::TrackBuilder& builder, AtomView const& view)
    {
      if (auto optYear = decodeUint16(atomTextView(view)); optYear)
      {
        (builder.metadata().*Setter)(*optYear);
      }
    }

    template<NumberSetter Setter>
    void handleTextNumber(library::TrackBuilder& builder, std::string_view value)
    {
      if (auto optNumber = decodeUint16(value); optNumber)
      {
        (builder.metadata().*Setter)(*optNumber);
      }
    }

    template<NumberSetter PrimarySetter, NumberSetter SecondarySetter>
    void handleSlashNumber(library::TrackBuilder& builder, std::string_view value)
    {
      auto const pair = parseSlashPair(value);

      if (pair.optPrimary)
      {
        (builder.metadata().*PrimarySetter)(*pair.optPrimary);
      }

      if (pair.optSecondary)
      {
        (builder.metadata().*SecondarySetter)(*pair.optSecondary);
      }
    }

    // Movement number/count atoms (©mvi/©mvc) store a binary big-endian integer payload,
    // unlike the text-encoded numeric atoms handled above.
    template<NumberSetter Setter>
    void handleIntegerNumber(library::TrackBuilder& builder, AtomView const& view)
    {
      constexpr std::uint32_t kVersionShift = 24;
      constexpr std::uint32_t kDataTypeMask = 0x00FFFFFFU;
      constexpr std::uint32_t kImplicitDataType = 0;
      constexpr std::uint32_t kIntegerDataType = 21;
      constexpr std::uint8_t kSignBitMask = 0x80U;

      auto const* const layout = dataAtomLayout(view);

      if (layout == nullptr)
      {
        return;
      }

      auto const type = layout->type.value();
      auto const version = type >> kVersionShift;
      auto const dataType = type & kDataTypeMask;
      auto const data = atomData(view);

      if (version != 0 || (dataType != kImplicitDataType && dataType != kIntegerDataType) ||
          (data.size() != 1 && data.size() != 2 && data.size() != 3 && data.size() != 4 && data.size() != 8))
      {
        return;
      }

      // MP4 INTEGER is signed. Movement fields are uint16_t, so reject negative
      // and out-of-range values instead of narrowing them modulo 2^16.
      if ((byteValue(data.front()) & kSignBitMask) != 0)
      {
        return;
      }

      std::uint64_t value = 0;

      for (auto const byte : data)
      {
        value = (value << 8U) | byteValue(byte);
      }

      if (value > std::numeric_limits<std::uint16_t>::max())
      {
        return;
      }

      (builder.metadata().*Setter)(static_cast<std::uint16_t>(value));
    }

    void handleCoverArt(library::TrackBuilder& builder, AtomView const& view)
    {
      // A covr atom may contain multiple child data boxes (the standard iTunes
      // encoding for multiple artwork). Iterate each data box and add a separate
      // cover entry. MP4 covr does not carry a picture-type role, so all entries
      // are treated as FrontCover.
      auto const bytes = view.bytes();
      constexpr std::size_t kOuterHeader = 8;       // outer covr: length(4) + type(4)
      constexpr std::size_t kDataChildMinSize = 16; // child data: length(4) + "data"(4) + type(4) + reserved(4)

      if (bytes.size() <= kOuterHeader)
      {
        return;
      }

      auto remaining = bytes.subspan(kOuterHeader);

      while (remaining.size() >= kDataChildMinSize)
      {
        // Read the child atom header: [length(4)][type(4)]
        auto const* const childLayout = utility::layout::view<AtomLayout>(remaining);
        std::size_t const childLength = childLayout->length.value();

        if (childLength < kDataChildMinSize || childLength > remaining.size())
        {
          break;
        }

        // Only process "data" children; skip unknown siblings
        if (std::string_view{childLayout->type.data(), childLayout->type.size()} == "data" &&
            childLength > kDataChildMinSize)
        {
          auto const payloadSize = childLength - kDataChildMinSize;
          auto const payload = remaining.subspan(kDataChildMinSize, payloadSize);
          builder.coverArt().add(library::PictureType::FrontCover, payload);
        }

        remaining = remaining.subspan(childLength);
      }
    }

    std::vector<std::string_view> readMdtaKeys(Atom const* keysNode)
    {
      auto keys = std::vector<std::string_view>{};

      if (keysNode == nullptr)
      {
        return keys;
      }

      auto const& view = utility::unsafeDowncast<AtomView const>(*keysNode);
      auto const bytes = view.bytes();
      constexpr std::size_t kFullBoxHeaderSize = 4;
      constexpr std::size_t kEntryCountSize = 4;
      constexpr std::size_t kKeyEntryHeaderSize = 8;
      constexpr std::size_t kKeyNamespaceSize = 4;

      if (bytes.size() < sizeof(AtomLayout) + kFullBoxHeaderSize + kEntryCountSize)
      {
        return keys;
      }

      auto offset = sizeof(AtomLayout) + kFullBoxHeaderSize;
      auto const keyCount = readBigEndianU32(bytes, offset);
      offset += kEntryCountSize;

      for (std::uint32_t keyIndex = 0; keyIndex < keyCount; ++keyIndex)
      {
        if (bytes.size() < offset + kKeyEntryHeaderSize)
        {
          break;
        }

        auto const entrySize = static_cast<std::size_t>(readBigEndianU32(bytes, offset));

        if (entrySize < kKeyEntryHeaderSize || entrySize > bytes.size() - offset)
        {
          break;
        }

        auto const keyNamespace =
          utility::bytes::stringView(bytes.subspan(offset + sizeof(std::uint32_t), kKeyNamespaceSize));

        if (keyNamespace == "mdta")
        {
          keys.push_back(
            utility::bytes::stringView(bytes.subspan(offset + kKeyEntryHeaderSize, entrySize - kKeyEntryHeaderSize)));
        }
        else
        {
          keys.emplace_back();
        }

        offset += entrySize;
      }

      return keys;
    }

    std::optional<std::size_t> mdtaKeyIndex(AtomView const& view)
    {
      auto const bytes = view.bytes();

      if (bytes.size() < sizeof(AtomLayout))
      {
        return std::nullopt;
      }

      auto const keyIndex = readBigEndianU32(bytes, sizeof(std::uint32_t));

      if (keyIndex == 0)
      {
        return std::nullopt;
      }

      return static_cast<std::size_t>(keyIndex - 1);
    }

    void handleMdtaMetadata(library::TrackBuilder& builder, std::string_view key, AtomView const& view)
    {
      if (auto const value = atomTextView(view); equalsAsciiCaseInsensitive(key, "title"))
      {
        builder.metadata().title(value);
      }
      else if (equalsAsciiCaseInsensitive(key, "artist"))
      {
        builder.metadata().artist(value);
      }
      else if (equalsAsciiCaseInsensitive(key, "album"))
      {
        builder.metadata().album(value);
      }
      else if (equalsAsciiCaseInsensitive(key, "album_artist") || equalsAsciiCaseInsensitive(key, "albumartist"))
      {
        builder.metadata().albumArtist(value);
      }
      else if (equalsAsciiCaseInsensitive(key, "genre"))
      {
        builder.metadata().genre(value);
      }
      else if (equalsAsciiCaseInsensitive(key, "composer"))
      {
        builder.metadata().composer(value);
      }
      else if (equalsAsciiCaseInsensitive(key, "conductor"))
      {
        builder.metadata().conductor(value);
      }
      else if (equalsAsciiCaseInsensitive(key, "ensemble") ||
               (equalsAsciiCaseInsensitive(key, "orchestra") && builder.metadata().ensemble().empty()))
      {
        builder.metadata().ensemble(value);
      }
      else if (equalsAsciiCaseInsensitive(key, "soloist"))
      {
        builder.metadata().soloist(value);
      }
      else if (equalsAsciiCaseInsensitive(key, "work") || equalsAsciiCaseInsensitive(key, "grouping"))
      {
        builder.metadata().work(value);
      }
      else if (equalsAsciiCaseInsensitive(key, "movementname") || equalsAsciiCaseInsensitive(key, "movement_name") ||
               equalsAsciiCaseInsensitive(key, "mvnm"))
      {
        builder.metadata().movement(value);
      }
      else if (equalsAsciiCaseInsensitive(key, "movement") || equalsAsciiCaseInsensitive(key, "mvin"))
      {
        handleSlashNumber<&library::TrackBuilder::MetadataBuilder::movementNumber,
                          &library::TrackBuilder::MetadataBuilder::movementTotal>(builder, value);
      }
      else if (equalsAsciiCaseInsensitive(key, "movementnumber") || equalsAsciiCaseInsensitive(key, "movement_number"))
      {
        handleTextNumber<&library::TrackBuilder::MetadataBuilder::movementNumber>(builder, value);
      }
      else if (equalsAsciiCaseInsensitive(key, "movementtotal") || equalsAsciiCaseInsensitive(key, "movement_total"))
      {
        handleTextNumber<&library::TrackBuilder::MetadataBuilder::movementTotal>(builder, value);
      }
      else if (equalsAsciiCaseInsensitive(key, "track") || equalsAsciiCaseInsensitive(key, "tracknumber"))
      {
        handleSlashNumber<&library::TrackBuilder::MetadataBuilder::trackNumber,
                          &library::TrackBuilder::MetadataBuilder::trackTotal>(builder, value);
      }
      else if (equalsAsciiCaseInsensitive(key, "disc") || equalsAsciiCaseInsensitive(key, "disk") ||
               equalsAsciiCaseInsensitive(key, "discnumber"))
      {
        handleSlashNumber<&library::TrackBuilder::MetadataBuilder::discNumber,
                          &library::TrackBuilder::MetadataBuilder::discTotal>(builder, value);
      }
      else if (equalsAsciiCaseInsensitive(key, "date") || equalsAsciiCaseInsensitive(key, "year"))
      {
        handleTextNumber<&library::TrackBuilder::MetadataBuilder::year>(builder, value);
      }
    }

    void handleMdtaAtom(library::TrackBuilder& builder, std::span<std::string_view const> keys, AtomView const& view)
    {
      auto const optKeyIndex = mdtaKeyIndex(view);

      if (!optKeyIndex || *optKeyIndex >= keys.size() || keys[*optKeyIndex].empty())
      {
        return;
      }

      handleMdtaMetadata(builder, keys[*optKeyIndex], view);
    }

    using AtomHandler = void (*)(library::TrackBuilder&, AtomView const&);

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#include "tag/mp4/AtomDispatch.h"
#pragma GCC diagnostic pop

    constexpr auto kIlstPath = std::to_array<std::string_view>({
      "root",
      "moov",
      "udta",
      "meta",
      "ilst",
    });

    constexpr auto kMdtaKeysPath = std::to_array<std::string_view>({
      "root",
      "moov",
      "udta",
      "meta",
      "keys",
    });

    constexpr auto kTrackMdhdPath = std::to_array<std::string_view>({
      "trak",
      "mdia",
      "mdhd",
    });

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

      // Only interpret the entry as an audio sample description when it is one of
      // the codecs we actually read properties from. Other sample-entry kinds share
      // a different field layout, so reading channel/sample data from them would be
      // meaningless (and the codec is otherwise reported as Unknown anyway).
      if (auto const entryType = utility::bytes::stringView(utility::bytes::view(entryLayout->type));
          entryType != "mp4a" && entryType != "alac")
      {
        return nullptr;
      }

      return utility::layout::view<AudioSampleEntryLayout>(entryBytes);
    }

    // Helper to extract audio properties from mdhd and stsd
    void extractAudioProperties(library::TrackBuilder& builder, RootAtom const& root, std::size_t fileSize)
    {
      auto const optSelection = findAudioTrack(root);

      if (!optSelection || optSelection->track == nullptr || optSelection->stsd == nullptr)
      {
        return;
      }

      auto const& track = *optSelection->track;

      // Get mdhd for sample rate and duration
      if (auto const* const mdhdNode = track.find(kTrackMdhdPath); mdhdNode != nullptr)
      {
        auto const& view = utility::unsafeDowncast<AtomView const>(*mdhdNode);

        if (auto const bytes = view.bytes();
            bytes.size() >= sizeof(MdhdAtomLayout) && std::to_integer<std::uint8_t>(bytes[sizeof(AtomLayout)]) == 0)
        {
          auto const& layout = view.layout<MdhdAtomLayout>();

          if (auto const timescale = layout.timescale.value(); timescale > 0)
          {
            builder.property().sampleRate(SampleRate{timescale});

            if (auto const duration = layout.duration.value(); duration > 0)
            {
              auto const trackDuration = std::chrono::milliseconds{
                (static_cast<std::uint64_t>(duration) * std::chrono::milliseconds::period::den) / timescale};

              if (trackDuration > std::chrono::milliseconds{0})
              {
                builder.property().duration(trackDuration).bitrate(Bitrate{bitrateFromBytes(fileSize, trackDuration)});
              }
            }
          }
        }
      }

      // Get stsd for channels and bit depth
      auto const* const audioLayout = firstAudioSampleEntry(*optSelection->stsd);

      if (audioLayout == nullptr)
      {
        return;
      }

      auto codec = AudioCodec::Unknown;

      if (optSelection->sampleEntryType == "alac")
      {
        codec = AudioCodec::Alac;
      }
      else if (optSelection->sampleEntryType == "mp4a")
      {
        codec = AudioCodec::Aac;
      }

      builder.property()
        .channels(Channels{static_cast<std::uint8_t>(audioLayout->channelCount.value())})
        .bitDepth(BitDepth{static_cast<std::uint8_t>(audioLayout->sampleSize.value())})
        .codec(codec);

      // Sample rate is a 16.16 fixed point, extract integer part
      // Only use if non-zero (ALAC may have 0 here, mdhd has correct rate)
      constexpr std::size_t kFixedPointShift = 16;

      if (auto const sampleRateFixed = audioLayout->sampleRate.value(); sampleRateFixed >> kFixedPointShift > 0)
      {
        builder.property().sampleRate(SampleRate{sampleRateFixed >> kFixedPointShift});
      }
    }
  } // namespace

  Result<library::TrackBuilder> File::loadTrackImpl() const
  {
    try
    {
      RootAtom const root = media::mp4::fromBuffer(utility::bytes::view(address(), size()));
      Atom const* const ilstNode = root.find(kIlstPath);
      auto const mdtaKeys = readMdtaKeys(root.find(kMdtaKeysPath));

      clearOwnedStrings();
      auto builder = library::TrackBuilder::createNew();

      if (ilstNode != nullptr)
      {
        ilstNode->visitChildren(
          [&](Atom const& atom)
          {
            auto const& view = utility::unsafeDowncast<AtomView const>(atom);
            std::string_view const type = atom.type();

            if (auto const* const entry = Mp4AtomDispatchTable::lookupAtomField(type.data(), type.size());
                entry != nullptr)
            {
              entry->handler(builder, view);
            }
            else
            {
              handleMdtaAtom(builder, mdtaKeys, view);
            }

            return true;
          });
      }

      extractAudioProperties(builder, root, size());

      return builder;
    }
    catch (detail::TagException const& ex)
    {
      return std::unexpected{ex.error()};
    }
  }

  Result<AudioPayload> File::audioPayloadImpl() const
  {
    try
    {
      RootAtom const root = media::mp4::fromBuffer(utility::bytes::view(address(), size()));
      auto optPayload = std::optional<AudioPayload>{};
      bool hasMultipleMdatAtoms = false;

      root.visitChildren(
        [&](Atom const& atom)
        {
          if (atom.type() != "mdat")
          {
            return true;
          }

          if (optPayload)
          {
            hasMultipleMdatAtoms = true;
            return false;
          }

          auto const& view = utility::unsafeDowncast<AtomView const>(atom);
          auto const atomBytes = view.bytes();

          if (atomBytes.size() < sizeof(AtomLayout))
          {
            return true;
          }

          auto const atomOffset = static_cast<std::size_t>(atomBytes.data() - static_cast<std::byte const*>(address()));
          auto const offset = atomOffset + sizeof(AtomLayout);
          optPayload = payloadRange(offset, atomBytes.size() - sizeof(AtomLayout));
          return true;
        });

      if (hasMultipleMdatAtoms)
      {
        return makeError(Error::Code::FormatRejected, "mp4 audio payload range requires a single mdat atom");
      }

      if (!optPayload)
      {
        return makeError(Error::Code::CorruptData, "mp4 file has no mdat audio payload");
      }

      return *optPayload;
    }
    catch (detail::TagException const& ex)
    {
      return std::unexpected{ex.error()};
    }
  }
} // namespace ao::tag::mp4
