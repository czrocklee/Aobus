// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "Reader.h"
#include "Frame.h"
#include "Layout.h"
#include <rs/tag/mpeg/File.h>

#include <cstring>

namespace rs::tag::mpeg::id3v2
{
  namespace
  {
    using TextSetter =
      rs::core::TrackBuilder::MetadataBuilder& (rs::core::TrackBuilder::MetadataBuilder::*)(std::string_view);
    using NumberSetter =
      rs::core::TrackBuilder::MetadataBuilder& (rs::core::TrackBuilder::MetadataBuilder::*)(std::uint16_t);

    void handlePicture(rs::core::TrackBuilder& builder, rs::tag::File const& owner, void const* data, std::size_t size);
    void handleTxxx(rs::core::TrackBuilder& builder, rs::tag::File const& owner, void const* data, std::size_t size);

    template<TextSetter Setter>
    void handleText(rs::core::TrackBuilder& builder, rs::tag::File const& owner, void const* data, std::size_t size)
    {
      auto view = V23TextFrameView{data, size};
      (builder.metadata().*Setter)(rs::tag::detail::stashOwnedString(owner, view.text()));
    }

    template<NumberSetter Setter>
    void handleNumber(rs::core::TrackBuilder& builder,
                      [[maybe_unused]] rs::tag::File const& owner,
                      void const* data,
                      std::size_t size)
    {
      auto view = V23TextFrameView{data, size};

      if (auto text = view.text(); !text.empty())
      {
        (builder.metadata().*Setter)(static_cast<std::uint16_t>(std::atoi(text.data())));
      }
    }

    template<NumberSetter PrimarySetter, NumberSetter SecondarySetter>
    void handleSlashNumber(rs::core::TrackBuilder& builder,
                           [[maybe_unused]] rs::tag::File const& owner,
                           void const* data,
                           std::size_t size)
    {
      auto view = V23TextFrameView{data, size};
      auto text = view.text();
      auto slashPos = text.find('/');
      (builder.metadata().*PrimarySetter)(static_cast<std::uint16_t>(std::atoi(text.data())));

      if (slashPos != std::string_view::npos)
      {
        (builder.metadata().*SecondarySetter)(static_cast<std::uint16_t>(std::atoi(text.data() + slashPos + 1)));
      }
    }

    void handlePicture(rs::core::TrackBuilder& builder,
                       [[maybe_unused]] rs::tag::File const& owner,
                       void const* data,
                       std::size_t size)
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

      std::size_t const imageSize = size - (ptr - frameData);
      builder.metadata().coverArtData(std::span{reinterpret_cast<std::byte const*>(ptr), imageSize});
    }

    void handleTxxx(rs::core::TrackBuilder& builder, rs::tag::File const& owner, void const* data, std::size_t size)
    {
      // TXXX frame layout:
      //   encoding byte (1)
      //   description (null-terminated)
      //   value
      auto view = V23TextFrameView{data, size};
      auto text = view.text();

      // TXXX format is "description\0value"

      if (auto nullPos = text.find('\0'); nullPos != std::string_view::npos)
      {
        auto key = text.substr(0, nullPos);
        auto value = text.substr(nullPos + 1);

        if (key == "rating")
        {
          builder.metadata().rating(static_cast<std::uint8_t>(std::atoi(value.data())));
        }
        else
        {
          // Store as custom pair - stash both strings
          auto const stashedKey = rs::tag::detail::stashOwnedString(owner, std::string{key});
          auto const stashedValue = rs::tag::detail::stashOwnedString(owner, std::string{value});
          builder.custom().add(stashedKey, stashedValue);
        }
      }
    }

#include "tag/mpeg/id3v2/FrameDispatch.h"

  } // namespace

  rs::core::TrackBuilder loadFrames(rs::tag::File const& owner,
                                    HeaderLayout const& header,
                                    void const* buffer,
                                    std::size_t size)
  {
    switch (header.majorVersion)
    {
      case 2: return rs::core::TrackBuilder::createNew();
      case 3:
      case 4:
      {
        auto builder = rs::core::TrackBuilder::createNew();
        auto frameIter = FrameViewIterator<V23FrameView>{buffer, size};
        auto frameEnd = FrameViewIterator<V23FrameView>{};

        for (; frameIter != frameEnd; ++frameIter)
        {
          std::string_view frameId = frameIter->id();

          if (auto const* entry = Id3v2FrameDispatchTable::lookupFrame(frameId.data(), frameId.size());
              entry != nullptr)
          {
            entry->handler(builder, owner, frameIter->data(), frameIter->size());
          }
        }

        return builder;
      }
      default: return rs::core::TrackBuilder::createNew();
    }
  }
} // namespace rs::tag::mpeg::id3v2
