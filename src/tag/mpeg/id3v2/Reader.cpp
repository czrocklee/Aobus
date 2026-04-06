// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "../../Decoder.h"
#include "Frame.h"
#include "Layout.h"
#include <rs/tag/ParsedTrack.h>

#include <cstring>
#include <span>

namespace rs::tag::mpeg::id3v2
{
  namespace
  {
    using Metadata = rs::core::TrackRecord::Metadata;
    using FrameHandler = void (*)(ParsedTrack&, void const*, std::size_t);

    template<auto Member>
    void handleText(ParsedTrack& parsed, void const* data, std::size_t size);

    void handleYear(ParsedTrack& parsed, void const* data, std::size_t size);
    void handleTrackNumber(ParsedTrack& parsed, void const* data, std::size_t size);
    void handleDiscNumber(ParsedTrack& parsed, void const* data, std::size_t size);
    void handlePicture(ParsedTrack& parsed, void const* data, std::size_t size);
    void handleTxxx(ParsedTrack& parsed, void const* data, std::size_t size);

#include "tag/mpeg/id3v2/FrameDispatch.h"

    template<auto Member>
    void handleText(ParsedTrack& parsed, void const* data, std::size_t size)
    {
      V23TextFrameView view{data, size};
      parsed.record.metadata.*Member = view.text();
    }

    void handleYear(ParsedTrack& parsed, void const* data, std::size_t size)
    {
      V23TextFrameView view{data, size};
      auto text = view.text();
      if (!text.empty())
      {
        parsed.record.metadata.year = static_cast<std::uint16_t>(std::atoi(text.data()));
      }
    }

    void handleTrackNumber(ParsedTrack& parsed, void const* data, std::size_t size)
    {
      V23TextFrameView view{data, size};
      auto text = view.text();
      auto slashPos = text.find('/');
      if (slashPos == std::string_view::npos)
      {
        parsed.record.metadata.trackNumber = static_cast<std::uint16_t>(std::atoi(text.data()));
      }
      else
      {
        parsed.record.metadata.trackNumber = static_cast<std::uint16_t>(std::atoi(text.data()));
        parsed.record.metadata.totalTracks = static_cast<std::uint16_t>(std::atoi(text.data() + slashPos + 1));
      }
    }

    void handleDiscNumber(ParsedTrack& parsed, void const* data, std::size_t size)
    {
      V23TextFrameView view{data, size};
      auto text = view.text();
      auto slashPos = text.find('/');
      if (slashPos == std::string_view::npos)
      {
        parsed.record.metadata.discNumber = static_cast<std::uint16_t>(std::atoi(text.data()));
      }
      else
      {
        parsed.record.metadata.discNumber = static_cast<std::uint16_t>(std::atoi(text.data()));
        parsed.record.metadata.totalDiscs = static_cast<std::uint16_t>(std::atoi(text.data() + slashPos + 1));
      }
    }

    void handlePicture(ParsedTrack& parsed, void const* data, std::size_t size)
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
      while (*ptr != '\0') { ++ptr; }
      ++ptr; // skip null terminator
      // Skip picture type
      ++ptr;
      // Skip description (null-terminated)
      while (*ptr != '\0') { ++ptr; }
      ++ptr; // skip null terminator

      std::size_t const imageSize = size - (ptr - frameData);
      parsed.embeddedCoverArt = viewBytes(std::span{reinterpret_cast<std::byte const*>(ptr), imageSize});
    }

    void handleTxxx(ParsedTrack& parsed, void const* data, std::size_t size)
    {
      // TXXX frame layout:
      //   encoding byte (1)
      //   description (null-terminated)
      //   value
      V23TextFrameView view{data, size};
      auto text = view.text();

      // TXXX format is "description\0value"
      auto nullPos = text.find('\0');
      if (nullPos != std::string_view::npos)
      {
        auto key = text.substr(0, nullPos);
        auto value = text.substr(nullPos + 1);

        if (key == "rating")
        {
          parsed.record.metadata.rating = static_cast<std::uint8_t>(std::atoi(value.data()));
        }
        else
        {
          // Store as custom pair
          parsed.record.custom.pairs.emplace_back(std::string{key}, std::string{value});
        }
      }
    }

    FrameHandler lookupFrameHandler(std::string_view id)
    {
      if (auto const* entry = Id3v2FrameDispatchTable::lookupFrame(id.data(), id.size()))
      {
        return entry->handler;
      }
      return nullptr;
    }
  } // namespace

  ParsedTrack loadFrames(HeaderLayout const& header, void const* buffer, std::size_t size)
  {
    switch (header.majorVersion)
    {
      case 2:
        return {};
      case 3:
      case 4:
      {
        ParsedTrack parsed;
        FrameViewIterator<V23FrameView> frameIter{buffer, size};
        FrameViewIterator<V23FrameView> frameEnd{};

        for (; frameIter != frameEnd; ++frameIter)
        {
          std::string_view frameId = frameIter->id();

          if (auto const handler = lookupFrameHandler(frameId))
          {
            handler(parsed, frameIter->data(), frameIter->size());
          }
        }

        return parsed;
      }
      default:
        return {};
    }
  }
} // namespace rs::tag::mpeg::id3v2
