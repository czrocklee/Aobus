// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "Reader.h"

#include "../../detail/Content.h"
#include "../../detail/Decoder.h"
#include "Frame.h"
#include "Layout.h"
#include <ao/PictureType.h>
#include <ao/utility/ByteView.h>

#include <algorithm>
#include <cctype>
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

    template<TextSetter Setter>
    void handleText(detail::ContentBuilder& builder, std::span<std::byte const> content, std::uint8_t /*version*/)
    {
      auto convertedStorage = std::string{};

      if (auto const optText = decodeFrameText(content, convertedStorage); optText && !optText->value.empty())
      {
        auto const value = optText->requiresOwnership ? builder.own(std::move(convertedStorage)) : optText->value;
        (builder.metadata().*Setter)(value);
      }
    }

    template<NumberSetter Setter>
    void handleNumber(detail::ContentBuilder& builder, std::span<std::byte const> content, std::uint8_t /*version*/)
    {
      auto convertedStorage = std::string{};

      if (auto const optText = decodeFrameText(content, convertedStorage); optText)
      {
        if (auto const optValue = decodeUint16(optText->value); optValue)
        {
          (builder.metadata().*Setter)(*optValue);
        }
      }
    }

    template<NumberSetter PrimarySetter, NumberSetter SecondarySetter>
    void handleSlashNumber(detail::ContentBuilder& builder,
                           std::span<std::byte const> content,
                           std::uint8_t /*version*/)
    {
      auto convertedStorage = std::string{};

      if (auto const optText = decodeFrameText(content, convertedStorage); optText)
      {
        auto const pair = parseSlashPair(optText->value);

        if (pair.optPrimary)
        {
          (builder.metadata().*PrimarySetter)(*pair.optPrimary);
        }

        if (pair.optSecondary)
        {
          (builder.metadata().*SecondarySetter)(*pair.optSecondary);
        }
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

    void handleTxxx(detail::ContentBuilder& builder, std::span<std::byte const> content, std::uint8_t /*version*/)
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

      auto const key = optText->value.substr(0, nullOffset);
      auto const value = optText->value.substr(nullOffset + 1);
      auto setter = TextSetter{};

      if (equalsAsciiCaseInsensitive(key, "work") || equalsAsciiCaseInsensitive(key, "grouping"))
      {
        setter = &detail::ContentBuilder::MetadataBuilder::work;
      }
      else if (equalsAsciiCaseInsensitive(key, "conductor"))
      {
        setter = &detail::ContentBuilder::MetadataBuilder::conductor;
      }
      else if (equalsAsciiCaseInsensitive(key, "ensemble") ||
               (equalsAsciiCaseInsensitive(key, "orchestra") && builder.metadata().ensemble().empty()))
      {
        setter = &detail::ContentBuilder::MetadataBuilder::ensemble;
      }
      else if (equalsAsciiCaseInsensitive(key, "soloist"))
      {
        setter = &detail::ContentBuilder::MetadataBuilder::soloist;
      }
      else if (equalsAsciiCaseInsensitive(key, "movementname") || equalsAsciiCaseInsensitive(key, "movement_name") ||
               equalsAsciiCaseInsensitive(key, "mvnm"))
      {
        setter = &detail::ContentBuilder::MetadataBuilder::movement;
      }
      else if (equalsAsciiCaseInsensitive(key, "movement") || equalsAsciiCaseInsensitive(key, "mvin"))
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
      }

      if (setter != nullptr)
      {
        auto const stableText = optText->requiresOwnership ? builder.own(std::move(convertedStorage)) : optText->value;
        (builder.metadata().*setter)(stableText.substr(nullOffset + 1));
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

    std::size_t offset = 0;

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

      if (auto const* const entry = Id3v2FrameDispatchTable::lookupFrame(frameId.data(), frameId.size());
          entry != nullptr)
      {
        entry->handler(builder, remaining.subspan(kFrameHeaderSize, *optContentSize), header.majorVersion);
      }

      offset += frameSize;
    }

    return builder;
  }
} // namespace ao::media::file::mpeg::id3v2
