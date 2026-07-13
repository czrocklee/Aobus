// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "File.h"

#include "../detail/Content.h"
#include "../detail/Decoder.h"
#include <ao/AudioCodec.h>
#include <ao/Error.h>
#include <ao/PictureType.h>
#include <ao/media/file/File.h>
#include <ao/media/mp4/Atom.h>
#include <ao/media/mp4/AtomLayout.h>
#include <ao/media/mp4/TrackSelection.h>
#include <ao/utility/ByteView.h>

#include <boost/endian/buffers.hpp>

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
#include <tuple>
#include <utility>
#include <vector>

namespace ao::media::file::mp4
{
  using namespace ao::media::mp4;

  namespace
  {
    using TextSetter =
      detail::ContentBuilder::MetadataBuilder& (detail::ContentBuilder::MetadataBuilder::*)(std::string_view);
    using NumberSetter =
      detail::ContentBuilder::MetadataBuilder& (detail::ContentBuilder::MetadataBuilder::*)(std::uint16_t);

    struct DataAtomBodyLayout final
    {
      boost::endian::big_uint32_buf_t dataLength;
      std::array<char, kAtomMagicSize> magic;
      boost::endian::big_uint32_buf_t type;
      boost::endian::big_uint32_buf_t reserved;
    };

    static_assert(sizeof(DataAtomBodyLayout) == sizeof(DataAtomLayout) - sizeof(AtomLayout));
    static_assert(alignof(DataAtomBodyLayout) == 1);
    static_assert(utility::layout::kIsBinaryLayoutType<DataAtomBodyLayout>);

    struct StsdBodyLayout final
    {
      boost::endian::big_uint32_buf_t versionAndFlags;
      boost::endian::big_uint32_buf_t entryCount;
    };

    static_assert(sizeof(StsdBodyLayout) == 8);
    static_assert(alignof(StsdBodyLayout) == 1);
    static_assert(utility::layout::kIsBinaryLayoutType<StsdBodyLayout>);

    struct AudioSampleEntryBodyLayout final
    {
      std::array<char, AudioSampleEntryLayout::kReserved1Size> reserved1;
      boost::endian::big_uint16_buf_t dataReferenceIndex;
      std::array<boost::endian::big_uint16_buf_t, 4> reserved2;
      boost::endian::big_uint16_buf_t channelCount;
      boost::endian::big_uint16_buf_t sampleSize;
      boost::endian::big_uint16_buf_t preDefined;
      boost::endian::big_uint16_buf_t reserved3;
      boost::endian::big_uint32_buf_t sampleRate;
    };

    static_assert(sizeof(AudioSampleEntryBodyLayout) == sizeof(AudioSampleEntryLayout) - sizeof(AtomLayout));
    static_assert(alignof(AudioSampleEntryBodyLayout) == 1);
    static_assert(utility::layout::kIsBinaryLayoutType<AudioSampleEntryBodyLayout>);

    struct MdhdVersion0BodyLayout final
    {
      boost::endian::big_uint32_buf_t versionAndFlags;
      boost::endian::big_uint32_buf_t creationTime;
      boost::endian::big_uint32_buf_t modificationTime;
      boost::endian::big_uint32_buf_t timescale;
      boost::endian::big_uint32_buf_t duration;
    };

    static_assert(sizeof(MdhdVersion0BodyLayout) == sizeof(MdhdAtomLayout) - sizeof(AtomLayout));
    static_assert(alignof(MdhdVersion0BodyLayout) == 1);
    static_assert(utility::layout::kIsBinaryLayoutType<MdhdVersion0BodyLayout>);

    DataAtomBodyLayout const* dataAtomLayout(AtomView const& view) noexcept
    {
      return utility::bytes::tryLayout<DataAtomBodyLayout>(view.payload());
    }

    std::optional<std::span<std::byte const>> atomData(AtomView const& view)
    {
      auto const payload = view.payload();
      auto const* const layout = dataAtomLayout(view);

      if (layout == nullptr)
      {
        return std::nullopt;
      }

      constexpr std::size_t kDataFieldsSize = sizeof(DataAtomBodyLayout);
      auto const dataLength = static_cast<std::size_t>(layout->dataLength.value());

      if (std::string_view{layout->magic.data(), layout->magic.size()} != "data" || dataLength < kDataFieldsSize ||
          dataLength != payload.size())
      {
        return std::nullopt;
      }

      return payload.subspan(sizeof(DataAtomBodyLayout));
    }

    std::optional<std::string_view> atomTextView(AtomView const& view)
    {
      if (auto const optData = atomData(view); optData)
      {
        return utility::bytes::stringView(*optData);
      }

      return std::nullopt;
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
      constexpr std::size_t kNumberPairPayloadSize = 6;
      auto const optData = atomData(view);

      if (!optData || optData->size() < kNumberPairPayloadSize)
      {
        return std::nullopt;
      }

      constexpr std::size_t kNumberOffset = 2;
      constexpr std::size_t kTotalOffset = 4;

      return NumberPair{
        .number = readBigEndianU16(*optData, kNumberOffset), .total = readBigEndianU16(*optData, kTotalOffset)};
    }

    void handleTrackNumbers(detail::ContentBuilder& builder, AtomView const& view)
    {
      if (auto optPair = atomNumberPair(view); optPair)
      {
        builder.metadata().trackNumber(optPair->number).trackTotal(optPair->total);
      }
    }

    void handleDiscNumbers(detail::ContentBuilder& builder, AtomView const& view)
    {
      if (auto optPair = atomNumberPair(view); optPair)
      {
        builder.metadata().discNumber(optPair->number).discTotal(optPair->total);
      }
    }

    template<TextSetter Setter>
    void handleText(detail::ContentBuilder& builder, AtomView const& view)
    {
      if (auto const optText = atomTextView(view); optText)
      {
        (builder.metadata().*Setter)(*optText);
      }
    }

    [[maybe_unused]] std::string_view atomPayloadAfter(std::span<std::byte const> bytes, std::size_t offset)
    {
      if (bytes.size() <= offset)
      {
        return {};
      }

      return utility::bytes::stringView(bytes.subspan(offset));
    }

    [[maybe_unused]] void handleFreeform(detail::ContentBuilder& builder, AtomView const& view)
    {
      auto mean = std::string_view{};
      auto name = std::string_view{};
      auto value = std::string_view{};
      bool malformed = false;

      auto const visitResult = visitChildren(view,
                                             [&](AtomView const& child)
                                             {
                                               if (child.type() == "mean" || child.type() == "name")
                                               {
                                                 constexpr std::size_t kFullBoxHeaderSize = sizeof(std::uint32_t);

                                                 if (child.payload().size() < kFullBoxHeaderSize)
                                                 {
                                                   malformed = true;
                                                   return false;
                                                 }

                                                 auto const text =
                                                   atomPayloadAfter(child.payload(), kFullBoxHeaderSize);

                                                 if (child.type() == "mean")
                                                 {
                                                   mean = text;
                                                 }
                                                 else
                                                 {
                                                   name = text;
                                                 }
                                               }
                                               else if (child.type() == "data")
                                               {
                                                 auto const optValue = atomTextView(child);

                                                 if (!optValue)
                                                 {
                                                   malformed = true;
                                                   return false;
                                                 }

                                                 value = *optValue;
                                               }

                                               return true;
                                             });

      if (!visitResult || malformed || mean != "com.apple.iTunes" || name.empty())
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
    void handleNumber(detail::ContentBuilder& builder, AtomView const& view)
    {
      if (auto const optText = atomTextView(view); optText)
      {
        if (auto const optYear = decodeUint16(*optText); optYear)
        {
          (builder.metadata().*Setter)(*optYear);
        }
      }
    }

    template<NumberSetter Setter>
    void handleTextNumber(detail::ContentBuilder& builder, std::string_view value)
    {
      if (auto optNumber = decodeUint16(value); optNumber)
      {
        (builder.metadata().*Setter)(*optNumber);
      }
    }

    template<NumberSetter PrimarySetter, NumberSetter SecondarySetter>
    void handleSlashNumber(detail::ContentBuilder& builder, std::string_view value)
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
    void handleIntegerNumber(detail::ContentBuilder& builder, AtomView const& view)
    {
      constexpr std::uint32_t kVersionShift = 24;
      constexpr std::uint32_t kDataTypeMask = 0x00FFFFFFU;
      constexpr std::uint32_t kImplicitDataType = 0;
      constexpr std::uint32_t kIntegerDataType = 21;
      constexpr std::uint8_t kSignBitMask = 0x80U;

      auto const* const layout = dataAtomLayout(view);
      auto const optData = atomData(view);

      if (layout == nullptr || !optData)
      {
        return;
      }

      auto const type = layout->type.value();
      auto const version = type >> kVersionShift;
      auto const dataType = type & kDataTypeMask;
      auto const data = *optData;

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

    void handleCoverArt(detail::ContentBuilder& builder, AtomView const& view)
    {
      // A covr atom may contain multiple child data boxes (the standard iTunes
      // encoding for multiple artwork). Iterate each data box and add a separate
      // cover entry. MP4 covr does not carry a picture-type role, so all entries
      // are treated as FrontCover.
      constexpr std::size_t kDataFieldsSize = 8; // type + reserved
      auto images = std::vector<std::span<std::byte const>>{};
      bool malformed = false;

      auto const visitResult = visitChildren(view,
                                             [&](AtomView const& child)
                                             {
                                               if (child.type() != "data")
                                               {
                                                 return true;
                                               }

                                               if (child.payload().size() <= kDataFieldsSize)
                                               {
                                                 malformed = true;
                                                 return false;
                                               }

                                               images.push_back(child.payload().subspan(kDataFieldsSize));
                                               return true;
                                             });

      if (!visitResult || malformed)
      {
        return;
      }

      for (auto const image : images)
      {
        builder.coverArt().add(PictureType::FrontCover, image);
      }
    }

    std::optional<std::vector<std::string_view>> readMdtaKeys(std::optional<AtomView> const& optKeysNode)
    {
      auto keys = std::vector<std::string_view>{};

      if (!optKeysNode)
      {
        return keys;
      }

      auto const bytes = optKeysNode->payload();
      constexpr std::size_t kFullBoxHeaderSize = 4;
      constexpr std::size_t kEntryCountSize = 4;
      constexpr std::size_t kKeyEntryHeaderSize = 8;
      constexpr std::size_t kKeyNamespaceSize = 4;

      if (bytes.size() < kFullBoxHeaderSize + kEntryCountSize)
      {
        return std::nullopt;
      }

      auto offset = kFullBoxHeaderSize;
      auto const keyCount = readBigEndianU32(bytes, offset);
      offset += kEntryCountSize;

      for (std::uint32_t keyIndex = 0; keyIndex < keyCount; ++keyIndex)
      {
        if (bytes.size() < offset + kKeyEntryHeaderSize)
        {
          return std::nullopt;
        }

        auto const entrySize = static_cast<std::size_t>(readBigEndianU32(bytes, offset));

        if (entrySize < kKeyEntryHeaderSize || entrySize > bytes.size() - offset)
        {
          return std::nullopt;
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

      if (offset != bytes.size())
      {
        return std::nullopt;
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

    void handleMdtaMetadataValue(detail::ContentBuilder& builder, std::string_view key, std::string_view value)
    {
      if (equalsAsciiCaseInsensitive(key, "title"))
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
        handleSlashNumber<&detail::ContentBuilder::MetadataBuilder::movementNumber,
                          &detail::ContentBuilder::MetadataBuilder::movementTotal>(builder, value);
      }
      else if (equalsAsciiCaseInsensitive(key, "movementnumber") || equalsAsciiCaseInsensitive(key, "movement_number"))
      {
        handleTextNumber<&detail::ContentBuilder::MetadataBuilder::movementNumber>(builder, value);
      }
      else if (equalsAsciiCaseInsensitive(key, "movementtotal") || equalsAsciiCaseInsensitive(key, "movement_total"))
      {
        handleTextNumber<&detail::ContentBuilder::MetadataBuilder::movementTotal>(builder, value);
      }
      else if (equalsAsciiCaseInsensitive(key, "track") || equalsAsciiCaseInsensitive(key, "tracknumber"))
      {
        handleSlashNumber<&detail::ContentBuilder::MetadataBuilder::trackNumber,
                          &detail::ContentBuilder::MetadataBuilder::trackTotal>(builder, value);
      }
      else if (equalsAsciiCaseInsensitive(key, "disc") || equalsAsciiCaseInsensitive(key, "disk") ||
               equalsAsciiCaseInsensitive(key, "discnumber"))
      {
        handleSlashNumber<&detail::ContentBuilder::MetadataBuilder::discNumber,
                          &detail::ContentBuilder::MetadataBuilder::discTotal>(builder, value);
      }
      else if (equalsAsciiCaseInsensitive(key, "date") || equalsAsciiCaseInsensitive(key, "year"))
      {
        handleTextNumber<&detail::ContentBuilder::MetadataBuilder::year>(builder, value);
      }
    }

    void handleMdtaMetadata(detail::ContentBuilder& builder, std::string_view key, AtomView const& view)
    {
      if (auto const optValue = atomTextView(view); optValue)
      {
        handleMdtaMetadataValue(builder, key, *optValue);
      }
    }

    void handleMdtaAtom(detail::ContentBuilder& builder, std::span<std::string_view const> keys, AtomView const& view)
    {
      auto const optKeyIndex = mdtaKeyIndex(view);

      if (!optKeyIndex || *optKeyIndex >= keys.size() || keys[*optKeyIndex].empty())
      {
        return;
      }

      handleMdtaMetadata(builder, keys[*optKeyIndex], view);
    }

    using AtomHandler = void (*)(detail::ContentBuilder&, AtomView const&);

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4267) // gperf's generated hash narrows size_t lengths
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#endif
#include "media/file/mp4/AtomDispatch.h"
#ifdef _MSC_VER
#pragma warning(pop)
#else
#pragma GCC diagnostic pop
#endif

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

    struct AudioSampleProperties final
    {
      std::uint16_t channels = 0;
      std::uint16_t bitDepth = 0;
      std::uint32_t sampleRate = 0;
    };

    std::optional<AudioSampleProperties> firstAudioSampleEntry(AtomView const& stsdView)
    {
      auto const* const stsdLayout = utility::bytes::tryLayout<StsdBodyLayout>(stsdView.payload());

      if (stsdLayout == nullptr || stsdLayout->entryCount.value() == 0)
      {
        return std::nullopt;
      }

      auto cursor = stsdView.children();
      auto entryResult = cursor.next();

      if (!entryResult || !*entryResult)
      {
        return std::nullopt;
      }

      auto const& entry = **entryResult;

      if (entry.type() != "mp4a" && entry.type() != "alac")
      {
        return std::nullopt;
      }

      auto const* const entryLayout = utility::bytes::tryLayout<AudioSampleEntryBodyLayout>(entry.payload());

      if (entryLayout == nullptr)
      {
        return std::nullopt;
      }

      return AudioSampleProperties{
        .channels = entryLayout->channelCount.value(),
        .bitDepth = entryLayout->sampleSize.value(),
        .sampleRate = entryLayout->sampleRate.value() >> 16U,
      };
    }

    // Helper to extract audio properties from mdhd and stsd
    void extractAudioProperties(detail::ContentBuilder& builder, AtomView const& root, std::size_t fileSize)
    {
      auto const selectionResult = findAudioTrack(root);

      if (!selectionResult)
      {
        return;
      }

      auto const& selection = *selectionResult;
      auto const& track = selection.track;

      // Get mdhd for sample rate and duration
      if (auto const mdhdResult = findAtom(track, kTrackMdhdPath); mdhdResult && *mdhdResult)
      {
        auto const& view = **mdhdResult;

        if (auto const payload = view.payload();
            payload.size() >= sizeof(MdhdVersion0BodyLayout) && std::to_integer<std::uint8_t>(payload[0]) == 0)
        {
          auto const* const layout = utility::layout::view<MdhdVersion0BodyLayout>(payload);

          if (auto const timescale = layout->timescale.value(); timescale > 0)
          {
            builder.property().sampleRate(SampleRate{timescale});

            if (auto const duration = layout->duration.value(); duration > 0)
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
      auto const optAudio = firstAudioSampleEntry(selection.stsd);

      if (!optAudio)
      {
        return;
      }

      auto codec = AudioCodec::Unknown;

      if (selection.sampleEntryType == "alac")
      {
        codec = AudioCodec::Alac;
      }
      else if (selection.sampleEntryType == "mp4a")
      {
        codec = AudioCodec::Aac;
      }

      builder.property()
        .channels(Channels{static_cast<std::uint8_t>(optAudio->channels)})
        .bitDepth(BitDepth{static_cast<std::uint8_t>(optAudio->bitDepth)})
        .codec(codec);

      // Only use a non-zero sample-entry rate (ALAC may rely on mdhd instead).
      if (auto const sampleRate = optAudio->sampleRate; sampleRate > 0)
      {
        builder.property().sampleRate(SampleRate{sampleRate});
      }
    }
  } // namespace

  Result<File::Index> const& File::index() const
  {
    if (_optIndexResult)
    {
      return *_optIndexResult;
    }

    auto const root = fromBuffer(bytes());
    auto optPayload = std::optional<PayloadView>{};
    std::size_t mdatCount = 0;

    auto const visitResult = visitChildren(root,
                                           [&](AtomView const& atom)
                                           {
                                             if (atom.type() != "mdat")
                                             {
                                               return true;
                                             }

                                             ++mdatCount;

                                             if (mdatCount > 1)
                                             {
                                               return false;
                                             }

                                             auto const payload = atom.payload();

                                             if (payload.empty())
                                             {
                                               return true;
                                             }

                                             auto const offset =
                                               static_cast<std::size_t>(payload.data() - bytes().data());
                                             optPayload = payloadRange(offset, payload.size());
                                             return true;
                                           });

    auto result = [&] -> Result<Index>
    {
      if (!visitResult)
      {
        return std::unexpected{visitResult.error()};
      }

      if (mdatCount > 1)
      {
        return makeError(Error::Code::FormatRejected, "mp4 audio payload range requires a single mdat atom");
      }

      if (!optPayload)
      {
        return makeError(Error::Code::CorruptData, "mp4 file has no mdat audio payload");
      }

      return Index{.root = root, .payload = *optPayload};
    };

    _optIndexResult.emplace(result());
    return *_optIndexResult;
  }

  Result<detail::Content> File::readContent() const
  {
    auto const& indexResult = index();

    if (!indexResult)
    {
      return std::unexpected{indexResult.error()};
    }

    auto const& root = indexResult->root;
    auto const ilstResult = findAtom(root, kIlstPath);
    auto const keysResult = findAtom(root, kMdtaKeysPath);
    auto const optMdtaKeys = readMdtaKeys(keysResult ? *keysResult : std::optional<AtomView>{});
    auto builder = detail::ContentBuilder::makeEmpty();

    if (ilstResult && *ilstResult)
    {
      // Metadata is optional. A malformed tail preserves already completed
      // sibling items, matching the bounded optional-evidence contract.
      std::ignore =
        visitChildren(**ilstResult,
                      [&](AtomView const& atom)
                      {
                        std::string_view const type = atom.type();

                        if (auto const* const entry = Mp4AtomDispatchTable::lookupAtomField(type.data(), type.size());
                            entry != nullptr)
                        {
                          entry->handler(builder, atom);
                        }
                        else if (optMdtaKeys)
                        {
                          handleMdtaAtom(builder, *optMdtaKeys, atom);
                        }

                        return true;
                      });
    }

    extractAudioProperties(builder, root, bytes().size());

    return std::move(builder).finish();
  }

  Result<PayloadView> File::audioPayload() const
  {
    auto const& indexResult = index();

    if (!indexResult)
    {
      return std::unexpected{indexResult.error()};
    }

    return indexResult->payload;
  }
} // namespace ao::media::file::mp4
