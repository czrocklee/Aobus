// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "Reader.h"

#include "Frame.h"
#include "Layout.h"
#include <ao/library/CoverArt.h>
#include <ao/library/TrackBuilder.h>
#include <ao/tag/TagFile.h>
#include <ao/utility/ByteView.h>

#include <charconv>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace ao::tag::mpeg::id3v2
{
  namespace
  {
    using TextSetter =
      library::TrackBuilder::MetadataBuilder& (library::TrackBuilder::MetadataBuilder::*)(std::string_view);
    using NumberSetter =
      library::TrackBuilder::MetadataBuilder& (library::TrackBuilder::MetadataBuilder::*)(std::uint16_t);
    constexpr std::uint8_t kId3v22MajorVersion = 2;
    constexpr std::uint8_t kId3v23MajorVersion = 3;
    constexpr std::uint8_t kId3v24MajorVersion = 4;

    template<typename T>
    std::optional<T> parseUnsigned(std::string_view text)
    {
      std::uint32_t value = 0;
      auto const* begin = text.data();
      auto const* end = begin + text.size();

      if (auto const [ptr, ec] = std::from_chars(begin, end, value);
          ec == std::errc{} && ptr != begin && value <= std::numeric_limits<T>::max())
      {
        return static_cast<T>(value);
      }

      return std::nullopt;
    }

    void handlePicture(library::TrackBuilder& builder,
                       TagFile const& owner,
                       void const* data,
                       std::size_t size,
                       std::uint8_t version);
    void handleTxxx(library::TrackBuilder& builder,
                    TagFile const& owner,
                    void const* data,
                    std::size_t size,
                    std::uint8_t version);

    // Decode a text frame using the version-appropriate view. The two views differ
    // in how the frame size is decoded (v2.3 plain big-endian vs v2.4 syncsafe), so
    // using the wrong one over-reads past the frame boundary on v2.4 tags.
    std::string decodeFrameText(std::uint8_t version, void const* data, std::size_t size)
    {
      if (version == kId3v24MajorVersion)
      {
        return V24TextFrameView{data, size}.text();
      }

      return V23TextFrameView{data, size}.text();
    }

    template<TextSetter Setter>
    void handleText(library::TrackBuilder& builder,
                    TagFile const& owner,
                    void const* data,
                    std::size_t size,
                    std::uint8_t version)
    {
      (builder.metadata().*Setter)(detail::stashOwnedString(owner, decodeFrameText(version, data, size)));
    }

    template<NumberSetter Setter>
    void handleNumber(library::TrackBuilder& builder,
                      TagFile const& /*owner*/,
                      void const* data,
                      std::size_t size,
                      std::uint8_t version)
    {
      if (auto const text = decodeFrameText(version, data, size); !text.empty())
      {
        if (auto const optValue = parseUnsigned<std::uint16_t>(text); optValue)
        {
          (builder.metadata().*Setter)(*optValue);
        }
      }
    }

    template<NumberSetter PrimarySetter, NumberSetter SecondarySetter>
    void handleSlashNumber(library::TrackBuilder& builder,
                           TagFile const& /*owner*/,
                           void const* data,
                           std::size_t size,
                           std::uint8_t version)
    {
      auto const text = decodeFrameText(version, data, size);
      auto const slashPos = text.find('/');

      if (auto const optValue = parseUnsigned<std::uint16_t>(text.substr(0, slashPos)); optValue)
      {
        (builder.metadata().*PrimarySetter)(*optValue);
      }

      if (slashPos != std::string_view::npos)
      {
        if (auto const optValue = parseUnsigned<std::uint16_t>(text.substr(slashPos + 1)); optValue)
        {
          (builder.metadata().*SecondarySetter)(*optValue);
        }
      }
    }

    void handlePicture(library::TrackBuilder& builder,
                       TagFile const& /*owner*/,
                       void const* data,
                       std::size_t size,
                       std::uint8_t /*version*/)
    {
      // APIC frame layout (after the common frame header, identical size for v2.3
      // and v2.4):
      //   encoding byte (1)
      //   MIME type (null-terminated string)
      //   picture type byte (1)
      //   description (null-terminated, encoding depends on encoding byte)
      //   image data (remainder)
      //
      // Every cursor advance below is bounded by frameEnd so that a truncated or
      // malformed frame can never walk past the buffer (release builds strip the
      // gsl contract checks, so these guards must be explicit).
      char const* const frameData = static_cast<char const*>(data);
      char const* const frameEnd = frameData + size;
      char const* ptr = frameData + sizeof(V23CommonFrameLayout);

      // Need at least the encoding byte past the common header.
      if (ptr >= frameEnd)
      {
        return;
      }

      ++ptr; // skip encoding byte

      // Skip MIME type
      while (ptr < frameEnd && *ptr != '\0')
      {
        ++ptr;
      }

      if (ptr >= frameEnd)
      {
        return;
      }

      ++ptr; // skip null terminator

      // Read picture type byte
      if (ptr >= frameEnd)
      {
        return;
      }

      auto const rawType = static_cast<std::uint8_t>(*ptr);
      auto const picType = rawType <= static_cast<std::uint8_t>(library::PictureType::PublisherLogo)
                             ? static_cast<library::PictureType>(rawType)
                             : library::PictureType::Other;
      ++ptr;

      // Skip description (null-terminated)
      while (ptr < frameEnd && *ptr != '\0')
      {
        ++ptr;
      }

      if (ptr >= frameEnd)
      {
        return;
      }

      ++ptr; // skip null terminator

      if (ptr >= frameEnd)
      {
        return;
      }

      std::size_t const imageSize = static_cast<std::size_t>(frameEnd - ptr);
      builder.coverArt().add(picType, utility::bytes::view(ptr, imageSize));
    }

    void handleTxxx(library::TrackBuilder& builder,
                    TagFile const& owner,
                    void const* data,
                    std::size_t size,
                    std::uint8_t version)
    {
      // TXXX frame layout:
      //   encoding byte (1)
      //   description (null-terminated)
      //   value
      auto text = decodeFrameText(version, data, size);

      // TXXX format is "description\0value"
      if (auto const nullPos = text.find('\0'); nullPos != std::string_view::npos)
      {
        auto const key = text.substr(0, nullPos);

        if (auto const value = text.substr(nullPos + 1);
            key == "work" || key == "WORK" || key == "grouping" || key == "GROUPING")
        {
          builder.metadata().work(detail::stashOwnedString(owner, std::string{value}));
        }
      }
    }

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#include "tag/mpeg/id3v2/FrameDispatch.h"
#pragma GCC diagnostic pop

    // Walk the frame list using the version-appropriate view (v2.3 and v2.4 encode
    // frame sizes differently) and route each known frame to its handler.
    template<typename FrameViewT>
    void dispatchFrames(library::TrackBuilder& builder,
                        TagFile const& owner,
                        void const* buffer,
                        std::size_t size,
                        std::uint8_t version)
    {
      for (auto frameIter = FrameViewIterator<FrameViewT>{buffer, size}, frameEnd = FrameViewIterator<FrameViewT>{};
           frameIter != frameEnd;
           ++frameIter)
      {
        std::string_view const frameId = frameIter->id();

        if (auto const* entry = Id3v2FrameDispatchTable::lookupFrame(frameId.data(), frameId.size()); entry != nullptr)
        {
          entry->handler(builder, owner, frameIter->data(), frameIter->size(), version);
        }
      }
    }
  } // namespace

  library::TrackBuilder loadFrames(TagFile const& owner,
                                   HeaderLayout const& header,
                                   void const* buffer,
                                   std::size_t size)
  {
    auto builder = library::TrackBuilder::createNew();

    switch (header.majorVersion)
    {
      case kId3v23MajorVersion: dispatchFrames<V23FrameView>(builder, owner, buffer, size, header.majorVersion); break;
      case kId3v24MajorVersion: dispatchFrames<V24FrameView>(builder, owner, buffer, size, header.majorVersion); break;
      case kId3v22MajorVersion:
      default: break;
    }

    return builder;
  }
} // namespace ao::tag::mpeg::id3v2
