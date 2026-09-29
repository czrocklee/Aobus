// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "Reader.h"

#include "../../detail/Content.h"
#include "../../detail/Decoder.h"
#include "Frame.h"
#include "Layout.h"
#include <ao/PictureType.h>
#include <ao/utility/ByteView.h>
#include <ao/utility/String.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace ao::media::file::mpeg::id3v2
{
  namespace
  {
    using TextSetter =
      detail::ContentBuilder::MetadataBuilder& (detail::ContentBuilder::MetadataBuilder::*)(std::string_view);
    using NumberSetter =
      detail::ContentBuilder::MetadataBuilder& (detail::ContentBuilder::MetadataBuilder::*)(std::uint16_t);
    constexpr std::uint8_t kId3v22MajorVersion = 2;
    constexpr std::uint8_t kId3v23MajorVersion = 3;
    constexpr std::uint8_t kId3v24MajorVersion = 4;
    constexpr std::uint8_t kSyncSafeHighBit = 0x80U;
    constexpr std::uint8_t kAsciiHighBit = 0x80U;
    constexpr std::size_t kFrameHeaderSize = sizeof(V23CommonFrameLayout);
    constexpr std::uint8_t kTagUnsyncFlag = 0x80U;
    constexpr std::uint8_t kExtendedHeaderFlag = 0x40U;
    constexpr std::size_t kExtendedHeaderSizeField = sizeof(EncodedSize);
    // Smallest legal extended header: v2.4 counts the size field itself, v2.3
    // counts the bytes after it, but both floors are six bytes.
    constexpr std::size_t kExtendedHeaderMinSize = 6;
    // A UTF-8-encoded U+FEFF byte order mark, which per-value BOMs become after
    // the whole-frame BOM is stripped and the NUL split runs.
    constexpr std::string_view kUtf8Bom{"\xEF\xBB\xBF", 3};
    // ID3v2.4 frame format flags (the second flag byte).
    constexpr std::uint8_t kGroupingIdentityFormatFlag = 0x40U;
    constexpr std::uint8_t kFrameCompressionFormatFlag = 0x08U;
    constexpr std::uint8_t kFrameEncryptionFormatFlag = 0x04U;
    constexpr std::uint8_t kFrameUnsyncFormatFlag = 0x02U;
    constexpr std::uint8_t kDataLengthIndicatorFlag = 0x01U;
    // ID3v2.3 frame format flags (the second flag byte).
    constexpr std::uint8_t kV23FrameCompressionFormatFlag = 0x80U;
    constexpr std::uint8_t kV23FrameEncryptionFormatFlag = 0x40U;
    constexpr std::uint8_t kV23GroupingIdentityFormatFlag = 0x20U;
    constexpr std::size_t kFrameFormatFlagsOffset = 9;
    constexpr std::size_t kDataLengthIndicatorSize = 4;
    constexpr std::uint8_t kUnsyncEscapeByte = 0xFFU;

    struct DecodedTextView final
    {
      std::string_view value;
      bool requiresOwnership = false;
    };

    void trimTrailingTerminators(std::string_view& value) noexcept
    {
      while (!value.empty() && value.back() == '\0')
      {
        value.remove_suffix(1);
      }
    }

    std::optional<DecodedTextView> decodeFrameText(std::span<std::byte const> content, std::string& convertedStorage)
    {
      convertedStorage.clear();

      if (content.empty())
      {
        return std::nullopt;
      }

      auto const rawEncoding = std::to_integer<std::uint8_t>(content.front());

      if (rawEncoding > static_cast<std::uint8_t>(Encoding::Utf8))
      {
        return std::nullopt;
      }

      auto const encoding = static_cast<Encoding>(rawEncoding);
      auto const encodedText = content.subspan(1);

      if (encoding == Encoding::Utf8)
      {
        auto value = text::utf8View(encodedText);
        trimTrailingTerminators(value);
        return DecodedTextView{.value = value};
      }

      if (encoding == Encoding::Latin1 &&
          std::ranges::all_of(
            encodedText, [](std::byte value) { return (std::to_integer<std::uint8_t>(value) & kAsciiHighBit) == 0; }))
      {
        auto value = utility::bytes::stringView(encodedText);
        trimTrailingTerminators(value);
        return DecodedTextView{.value = value};
      }

      convertedStorage = convertToUtf8(encodedText, encoding);

      while (!convertedStorage.empty() && convertedStorage.back() == '\0')
      {
        convertedStorage.pop_back();
      }

      return DecodedTextView{.value = convertedStorage, .requiresOwnership = true};
    }

    // Applies the text once per value. ID3v2.3 strings end at their first
    // terminator; ID3v2.4 frames may hold several NUL-separated values, which
    // apply in order like repeated frames, so the last value wins.
    template<typename Apply>
    void forEachTextValue(std::string_view text, std::uint8_t version, Apply apply)
    {
      if (version != kId3v24MajorVersion)
      {
        auto const terminatorOffset = text.find('\0');
        apply(terminatorOffset == std::string_view::npos ? text : text.substr(0, terminatorOffset));
        return;
      }

      std::size_t offset = 0;

      while (offset < text.size())
      {
        auto const terminatorOffset = text.find('\0', offset);
        auto value = text.substr(
          offset, terminatorOffset == std::string_view::npos ? std::string_view::npos : terminatorOffset - offset);

        // A v2.4 multi-value frame may give each value its own BOM. The
        // whole-frame BOM was already stripped during conversion, but interior
        // BOMs survive the NUL split as a leading UTF-8-encoded U+FEFF.
        if (value.starts_with(kUtf8Bom))
        {
          value.remove_prefix(kUtf8Bom.size());
        }

        apply(value);

        if (terminatorOffset == std::string_view::npos)
        {
          return;
        }

        offset = terminatorOffset + 1;
      }
    }

    template<TextSetter Setter>
    void handleText(detail::ContentBuilder& builder, std::span<std::byte const> content, std::uint8_t version)
    {
      auto convertedStorage = std::string{};

      if (auto const optText = decodeFrameText(content, convertedStorage); optText && !optText->value.empty())
      {
        // Own the storage once before splitting so every value segment stays stable.
        auto const stableText = optText->requiresOwnership ? builder.own(std::move(convertedStorage)) : optText->value;

        forEachTextValue(stableText,
                         version,
                         [&](std::string_view value)
                         {
                           if (!value.empty())
                           {
                             (builder.metadata().*Setter)(value);
                           }
                         });
      }
    }

    void handleYear(detail::ContentBuilder& builder, std::span<std::byte const> content, std::uint8_t version)
    {
      auto convertedStorage = std::string{};

      if (auto const optText = decodeFrameText(content, convertedStorage); optText && !optText->value.empty())
      {
        auto const stableText = optText->requiresOwnership ? builder.own(std::move(convertedStorage)) : optText->value;

        forEachTextValue(stableText,
                         version,
                         [&](std::string_view value)
                         {
                           if (auto const optYear = decodeYear(value); optYear)
                           {
                             builder.metadata().year(*optYear);
                           }
                         });
      }
    }

    template<NumberSetter PrimarySetter, NumberSetter SecondarySetter>
    void handleSlashNumber(detail::ContentBuilder& builder, std::span<std::byte const> content, std::uint8_t version)
    {
      auto convertedStorage = std::string{};

      if (auto const optText = decodeFrameText(content, convertedStorage); optText && !optText->value.empty())
      {
        auto const stableText = optText->requiresOwnership ? builder.own(std::move(convertedStorage)) : optText->value;

        forEachTextValue(stableText,
                         version,
                         [&](std::string_view value)
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
                         });
      }
    }

    std::optional<std::size_t> findTerminator(std::span<std::byte const> bytes,
                                              std::size_t offset,
                                              Encoding encoding) noexcept
    {
      auto const unit = encoding == Encoding::Ucs2 || encoding == Encoding::Utf16Be ? 2U : 1U;

      for (std::size_t index = offset; index + unit <= bytes.size(); index += unit)
      {
        if (std::all_of(bytes.begin() + static_cast<std::ptrdiff_t>(index),
                        bytes.begin() + static_cast<std::ptrdiff_t>(index + unit),
                        [](std::byte value) { return value == std::byte{}; }))
        {
          return index + unit;
        }
      }

      return std::nullopt;
    }

    void handlePicture(detail::ContentBuilder& builder, std::span<std::byte const> content, std::uint8_t /*version*/)
    {
      if (content.empty())
      {
        return;
      }

      auto const rawEncoding = std::to_integer<std::uint8_t>(content.front());

      if (rawEncoding > static_cast<std::uint8_t>(Encoding::Utf8))
      {
        return;
      }

      auto const encoding = static_cast<Encoding>(rawEncoding);
      auto const mime = content.subspan(1);
      auto const mimeEnd = std::ranges::find(mime, std::byte{});

      if (mimeEnd == mime.end())
      {
        return;
      }

      auto const typeOffset = static_cast<std::size_t>(mimeEnd - mime.begin()) + 2;

      if (typeOffset >= content.size())
      {
        return;
      }

      auto const rawType = std::to_integer<std::uint8_t>(content[typeOffset]);
      auto const optDescriptionEnd = findTerminator(content, typeOffset + 1, encoding);

      if (!optDescriptionEnd || *optDescriptionEnd >= content.size())
      {
        return;
      }

      auto const pictureType = rawType <= static_cast<std::uint8_t>(PictureType::PublisherLogo)
                                 ? static_cast<PictureType>(rawType)
                                 : PictureType::Other;
      builder.coverArt().add(pictureType, content.subspan(*optDescriptionEnd));
    }

    bool isEqualIgnoringAsciiCase(std::string_view lhs, std::string_view rhs) noexcept
    {
      if (lhs.size() != rhs.size())
      {
        return false;
      }

      for (std::size_t index = 0; index < lhs.size(); ++index)
      {
        if (utility::toAsciiLower(lhs[index]) != utility::toAsciiLower(rhs[index]))
        {
          return false;
        }
      }

      return true;
    }

    void handleTxxx(detail::ContentBuilder& builder, std::span<std::byte const> content, std::uint8_t version)
    {
      auto convertedStorage = std::string{};
      auto const optText = decodeFrameText(content, convertedStorage);

      if (!optText)
      {
        return;
      }

      auto const nullOffset = optText->value.find('\0');

      if (nullOffset == std::string::npos)
      {
        return;
      }

      // Own the storage once before splitting so every value segment stays stable.
      auto const stableText = optText->requiresOwnership ? builder.own(std::move(convertedStorage)) : optText->value;
      auto const key = stableText.substr(0, nullOffset);
      auto const valueText = stableText.substr(nullOffset + 1);
      auto setter = TextSetter{};

      if (isEqualIgnoringAsciiCase(key, "work") || isEqualIgnoringAsciiCase(key, "grouping"))
      {
        setter = &detail::ContentBuilder::MetadataBuilder::work;
      }
      else if (isEqualIgnoringAsciiCase(key, "conductor"))
      {
        setter = &detail::ContentBuilder::MetadataBuilder::conductor;
      }
      else if (isEqualIgnoringAsciiCase(key, "ensemble") ||
               (isEqualIgnoringAsciiCase(key, "orchestra") && builder.metadata().ensemble().empty()))
      {
        setter = &detail::ContentBuilder::MetadataBuilder::ensemble;
      }
      else if (isEqualIgnoringAsciiCase(key, "soloist"))
      {
        setter = &detail::ContentBuilder::MetadataBuilder::soloist;
      }
      else if (isEqualIgnoringAsciiCase(key, "movementname") || isEqualIgnoringAsciiCase(key, "movement_name") ||
               isEqualIgnoringAsciiCase(key, "mvnm"))
      {
        setter = &detail::ContentBuilder::MetadataBuilder::movement;
      }
      else if (isEqualIgnoringAsciiCase(key, "movement") || isEqualIgnoringAsciiCase(key, "mvin"))
      {
        forEachTextValue(valueText,
                         version,
                         [&](std::string_view value)
                         {
                           auto const pair = parseSlashPair(value);

                           if (pair.optPrimary)
                           {
                             builder.metadata().movementNumber(*pair.optPrimary);
                           }

                           if (pair.optSecondary)
                           {
                             builder.metadata().movementTotal(*pair.optSecondary);
                           }
                         });
      }

      if (setter != nullptr)
      {
        forEachTextValue(valueText,
                         version,
                         [&](std::string_view value)
                         {
                           if (!value.empty())
                           {
                             (builder.metadata().*setter)(value);
                           }
                         });
      }
    }

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4267)
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#endif
#include "media/file/mpeg/id3v2/FrameDispatch.h"
#ifdef _MSC_VER
#pragma warning(pop)
#else
#pragma GCC diagnostic pop
#endif

    std::optional<std::size_t> frameContentSize(std::span<std::byte const> frame, std::uint8_t version) noexcept
    {
      if (frame.size() < kFrameHeaderSize)
      {
        return std::nullopt;
      }

      if (version == kId3v24MajorVersion)
      {
        auto const encoded = utility::layout::view<V24CommonFrameLayout>(frame)->size;

        if (std::ranges::any_of(encoded.data, [](std::uint8_t value) { return (value & kSyncSafeHighBit) != 0; }))
        {
          return std::nullopt;
        }

        return decodeSize(encoded);
      }

      return utility::layout::view<V23CommonFrameLayout>(frame)->size.value();
    }

    // Replaces unsynchronisation escape pairs (FF 00) with the encoded FF byte
    // and keeps the deunsynchronised copy alive in the builder, so borrowed
    // views into it (picture data, zero-copy text) outlive readFrames. A body
    // without a single escape pair keeps the zero-copy view: the input is
    // always file-backed or builder-owned and stays stable either way.
    std::span<std::byte const> ownDeunsynchronized(detail::ContentBuilder& builder, std::span<std::byte const> bytes)
    {
      auto const isEscapePair = [](std::byte const first, std::byte const second)
      { return first == std::byte{kUnsyncEscapeByte} && second == std::byte{}; };

      if (std::ranges::adjacent_find(bytes, isEscapePair) == bytes.end())
      {
        return bytes;
      }

      auto deunsynchronized = std::string{};
      deunsynchronized.reserve(bytes.size());

      for (std::size_t index = 0; index < bytes.size(); ++index)
      {
        deunsynchronized.push_back(std::to_integer<char>(bytes[index]));

        if (bytes[index] == std::byte{kUnsyncEscapeByte} && index + 1 < bytes.size() && bytes[index + 1] == std::byte{})
        {
          ++index;
        }
      }

      return utility::bytes::view(builder.own(std::move(deunsynchronized)));
    }

    // Byte count to skip before the first frame, or nullopt when the extended
    // header is truncated or malformed. The v2.3 size is a big-endian count
    // that excludes the size field itself; the v2.4 size is syncsafe and
    // includes the size field.
    std::optional<std::size_t> extendedHeaderSkip(std::span<std::byte const> bytes, std::uint8_t version) noexcept
    {
      if (bytes.size() < kExtendedHeaderSizeField)
      {
        return std::nullopt;
      }

      if (version == kId3v24MajorVersion)
      {
        auto const* const encoded = utility::layout::view<EncodedSize>(bytes);

        if (std::ranges::any_of(encoded->data, [](std::uint8_t value) { return (value & kSyncSafeHighBit) != 0; }))
        {
          return std::nullopt;
        }

        auto const size = decodeSize(*encoded);

        if (size < kExtendedHeaderMinSize || size > bytes.size())
        {
          return std::nullopt;
        }

        return size;
      }

      auto const size = utility::layout::view<boost::endian::big_uint32_buf_t>(bytes)->value();

      if (size < kExtendedHeaderMinSize || size > bytes.size() - kExtendedHeaderSizeField)
      {
        return std::nullopt;
      }

      return kExtendedHeaderSizeField + static_cast<std::size_t>(size);
    }

    // Returns the frame content a handler should see, deunsynchronising a
    // v2.4 frame, skipping its group identity byte and data length
    // indicator as needed, and applying the v2.3 format flags; nullopt means
    // the frame must not be dispatched.
    std::optional<std::span<std::byte const>> prepareFrameData(detail::ContentBuilder& builder,
                                                               std::span<std::byte const> frameData,
                                                               std::uint8_t formatFlags,
                                                               std::uint8_t majorVersion,
                                                               bool tagUnsynced)
    {
      if (majorVersion != kId3v24MajorVersion)
      {
        // v2.3 has no per-frame unsynchronisation or data length indicator;
        // the tag-level deunsynchronisation already covered the body.
        // Compressed and encrypted frames have no decoder here, so they are
        // skipped instead of dispatching undecodable bytes; the tag walk
        // keeps going like for any other dropped frame.
        if ((formatFlags & (kV23FrameCompressionFormatFlag | kV23FrameEncryptionFormatFlag)) != 0)
        {
          return std::nullopt;
        }

        // A group identity is a 1-byte prefix on the frame data.
        if ((formatFlags & kV23GroupingIdentityFormatFlag) != 0)
        {
          frameData = frameData.empty() ? std::span<std::byte const>{} : frameData.subspan(1);
        }

        return frameData;
      }

      // Compressed and encrypted frames have no decoder here, so they are
      // skipped instead of dispatching undecodable bytes; the tag walk keeps
      // going like for any other dropped frame.
      if ((formatFlags & (kFrameCompressionFormatFlag | kFrameEncryptionFormatFlag)) != 0)
      {
        return std::nullopt;
      }

      // ID3v2.4 frame sizes count stored bytes, so unsynchronised frames are
      // deunsynchronised before a handler sees the content.
      if (tagUnsynced || (formatFlags & kFrameUnsyncFormatFlag) != 0)
      {
        frameData = ownDeunsynchronized(builder, frameData);
      }

      // A group identity is a 1-byte prefix on the frame data.
      if ((formatFlags & kGroupingIdentityFormatFlag) != 0)
      {
        frameData = frameData.empty() ? std::span<std::byte const>{} : frameData.subspan(1);
      }

      // A data length indicator is a 4-byte syncsafe prefix on the frame
      // data. Its value is advisory, so it is not cross-checked against the
      // content (writers get it wrong); a non-syncsafe encoding is malformed
      // and skips the frame.
      if ((formatFlags & kDataLengthIndicatorFlag) != 0)
      {
        if (frameData.size() < kDataLengthIndicatorSize)
        {
          return std::span<std::byte const>{};
        }

        auto const& encoded = utility::layout::view<EncodedSize>(frameData.first(kDataLengthIndicatorSize))->data;

        if (std::ranges::any_of(encoded, [](std::uint8_t value) { return (value & kSyncSafeHighBit) != 0; }))
        {
          return std::nullopt;
        }

        frameData = frameData.subspan(kDataLengthIndicatorSize);
      }

      return frameData;
    }
  } // namespace

  std::optional<detail::ContentBuilder> readFrames(HeaderLayout const& header, std::span<std::byte const> bytes)
  {
    auto builder = detail::ContentBuilder::makeEmpty();

    if (header.majorVersion == kId3v22MajorVersion)
    {
      return builder;
    }

    if (header.majorVersion != kId3v23MajorVersion && header.majorVersion != kId3v24MajorVersion)
    {
      return std::nullopt;
    }

    // ID3v2.3 unsynchronisation applies to the whole tag body; ID3v2.4 applies
    // it per frame instead. Frame sizes are then read from the deunsynchronised
    // body, i.e. they count deunsynchronised bytes: the v2.3 spec is ambiguous
    // about which byte count a size holds, and TagLib and mutagen read the
    // sizes the same way.
    if (header.majorVersion == kId3v23MajorVersion && (header.flags & kTagUnsyncFlag) != 0)
    {
      bytes = ownDeunsynchronized(builder, bytes);
    }

    std::size_t offset = 0;

    // An extended header (flag 0x40) precedes the frame list.
    if ((header.flags & kExtendedHeaderFlag) != 0)
    {
      auto const optSkip = extendedHeaderSkip(bytes, header.majorVersion);

      if (!optSkip)
      {
        return std::nullopt;
      }

      offset = *optSkip;
    }

    // A v2.4 tag-level unsync flag means every frame in the tag is unsynchronised.
    auto const tagUnsynced = (header.flags & kTagUnsyncFlag) != 0;

    while (offset < bytes.size())
    {
      auto const remaining = bytes.subspan(offset);

      if (std::ranges::all_of(remaining, [](std::byte value) { return value == std::byte{}; }))
      {
        return builder;
      }

      if (remaining.size() < kFrameHeaderSize)
      {
        return std::nullopt;
      }

      auto const frameId = utility::bytes::stringView(remaining.first(4));
      auto const optContentSize = frameContentSize(remaining, header.majorVersion);

      if (!optContentSize || *optContentSize > remaining.size() - kFrameHeaderSize)
      {
        return std::nullopt;
      }

      auto const frameSize = kFrameHeaderSize + *optContentSize;
      auto const frameData = remaining.subspan(kFrameHeaderSize, *optContentSize);

      if (auto const* const entry = Id3v2FrameDispatchTable::lookupFrame(frameId.data(), frameId.size());
          entry != nullptr)
      {
        // Preparing only handled frames keeps unknown frames in
        // unsynchronised tags from allocating a deunsynchronised copy.
        auto const formatFlags = std::to_integer<std::uint8_t>(remaining[kFrameFormatFlagsOffset]);
        auto const optPreparedData =
          prepareFrameData(builder, frameData, formatFlags, header.majorVersion, tagUnsynced);

        if (optPreparedData)
        {
          entry->handler(builder, *optPreparedData, header.majorVersion);
        }
      }

      offset += frameSize;
    }

    return builder;
  }
} // namespace ao::media::file::mpeg::id3v2
