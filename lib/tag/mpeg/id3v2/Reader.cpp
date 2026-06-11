// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "Reader.h"

#include "Frame.h"
#include "Layout.h"
#include <ao/library/TrackBuilder.h>
#include <ao/tag/TagFile.h>
#include <ao/utility/ByteView.h>

#include <charconv>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
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

    void handlePicture(library::TrackBuilder& builder, TagFile const& owner, void const* data, std::size_t size);
    void handleTxxx(library::TrackBuilder& builder, TagFile const& owner, void const* data, std::size_t size);

    template<TextSetter Setter>
    void handleText(library::TrackBuilder& builder, TagFile const& owner, void const* data, std::size_t size)
    {
      auto view = V23TextFrameView{data, size};
      (builder.metadata().*Setter)(detail::stashOwnedString(owner, view.text()));
    }

    template<NumberSetter Setter>
    void handleNumber(library::TrackBuilder& builder, TagFile const& /*owner*/, void const* data, std::size_t size)
    {
      auto const view = V23TextFrameView{data, size};

      if (auto const text = view.text(); !text.empty())
      {
        if (auto const optValue = parseUnsigned<std::uint16_t>(text); optValue)
        {
          (builder.metadata().*Setter)(*optValue);
        }
      }
    }

    template<NumberSetter PrimarySetter, NumberSetter SecondarySetter>
    void handleSlashNumber(library::TrackBuilder& builder, TagFile const& /*owner*/, void const* data, std::size_t size)
    {
      auto const view = V23TextFrameView{data, size};
      auto const text = view.text();
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

    void handlePicture(library::TrackBuilder& builder, TagFile const& /*owner*/, void const* data, std::size_t size)
    {
      // APIC frame layout (after V23CommonFrameLayout header):
      //   encoding byte (1)
      //   MIME type (null-terminated string)
      //   picture type byte (1)
      //   description (null-terminated, encoding depends on encoding byte)
      //   image data (remainder)
      char const* frameData = static_cast<char const*>(data);
      char const* ptr = frameData + sizeof(V23CommonFrameLayout);

      // Skip encoding byte
      ++ptr;
      // Skip MIME type
      while (*ptr != '\0')
      {
        ++ptr;
      }

      ++ptr; // skip null terminator
      // Skip picture type
      ++ptr;
      // Skip description (null-terminated)
      while (*ptr != '\0')
      {
        ++ptr;
      }

      ++ptr; // skip null terminator

      auto const offset = static_cast<std::size_t>(ptr - frameData);
      std::size_t const imageSize = size - offset;
      builder.metadata().coverArtData(utility::bytes::view(ptr, imageSize));
    }

    void handleTxxx(library::TrackBuilder& builder, TagFile const& owner, void const* data, std::size_t size)
    {
      // TXXX frame layout:
      //   encoding byte (1)
      //   description (null-terminated)
      //   value
      auto view = V23TextFrameView{data, size};
      auto text = view.text();

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
  } // namespace

  library::TrackBuilder loadFrames(TagFile const& owner,
                                   HeaderLayout const& header,
                                   void const* buffer,
                                   std::size_t size)
  {
    switch (header.majorVersion)
    {
      case kId3v22MajorVersion: return library::TrackBuilder::createNew();
      case kId3v23MajorVersion:
      case kId3v24MajorVersion:
      {
        auto builder = library::TrackBuilder::createNew();
        auto frameIter = FrameViewIterator<V23FrameView>{buffer, size};
        auto frameEnd = FrameViewIterator<V23FrameView>{};

        for (; frameIter != frameEnd; ++frameIter)
        {
          std::string_view const frameId = frameIter->id();

          if (auto const* entry = Id3v2FrameDispatchTable::lookupFrame(frameId.data(), frameId.size());
              entry != nullptr)
          {
            entry->handler(builder, owner, frameIter->data(), frameIter->size());
          }
        }

        return builder;
      }
      default: return library::TrackBuilder::createNew();
    }
  }
} // namespace ao::tag::mpeg::id3v2
