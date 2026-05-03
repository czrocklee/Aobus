// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "Reader.h"
#include "Frame.h"
#include "Layout.h"
#include <ao/tag/mpeg/File.h>
#include <ao/utility/ByteView.h>

#include <charconv>
#include <cstring>
#include <limits>
#include <optional>

namespace ao::tag::mpeg::id3v2
{
  namespace
  {
    using TextSetter =
      ao::library::TrackBuilder::MetadataBuilder& (ao::library::TrackBuilder::MetadataBuilder::*)(std::string_view);
    using NumberSetter =
      ao::library::TrackBuilder::MetadataBuilder& (ao::library::TrackBuilder::MetadataBuilder::*)(std::uint16_t);

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

    void handlePicture(ao::library::TrackBuilder& builder,
                       ao::tag::File const& owner,
                       void const* data,
                       std::size_t size);
    void handleTxxx(ao::library::TrackBuilder& builder, ao::tag::File const& owner, void const* data, std::size_t size);

    template<TextSetter Setter>
    void handleText(ao::library::TrackBuilder& builder, ao::tag::File const& owner, void const* data, std::size_t size)
    {
      auto view = V23TextFrameView{data, size};
      (builder.metadata().*Setter)(ao::tag::detail::stashOwnedString(owner, view.text()));
    }

    template<NumberSetter Setter>
    void handleNumber(ao::library::TrackBuilder& builder,
                      [[maybe_unused]] ao::tag::File const& owner,
                      void const* data,
                      std::size_t size)
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
    void handleSlashNumber(ao::library::TrackBuilder& builder,
                           [[maybe_unused]] ao::tag::File const& owner,
                           void const* data,
                           std::size_t size)
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

    void handlePicture(ao::library::TrackBuilder& builder,
                       [[maybe_unused]] ao::tag::File const& owner,
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
      builder.metadata().coverArtData(ao::utility::bytes::view(ptr, imageSize));
    }

    void handleTxxx(ao::library::TrackBuilder& builder, ao::tag::File const& owner, void const* data, std::size_t size)
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
        auto const value = text.substr(nullPos + 1);

        if (key == "rating")
        {
          if (auto const optRating = parseUnsigned<std::uint8_t>(value); optRating)
          {
            builder.metadata().rating(*optRating);
          }
        }
        else if (key == "work" || key == "WORK" || key == "grouping" || key == "GROUPING")
        {
          builder.metadata().work(ao::tag::detail::stashOwnedString(owner, std::string{value}));
        }
        else
        {
          // Store as custom pair - stash both strings
          auto const stashedKey = ao::tag::detail::stashOwnedString(owner, std::string{key});
          auto const stashedValue = ao::tag::detail::stashOwnedString(owner, std::string{value});
          builder.custom().add(stashedKey, stashedValue);
        }
      }
    }

#include "tag/mpeg/id3v2/FrameDispatch.h"
  } // namespace

  ao::library::TrackBuilder loadFrames(ao::tag::File const& owner,
                                       HeaderLayout const& header,
                                       void const* buffer,
                                       std::size_t size)
  {
    switch (header.majorVersion)
    {
      case 2: return ao::library::TrackBuilder::createNew();
      case 3: // NOLINT(readability-magic-numbers)
      case 4:
      {
        auto builder = ao::library::TrackBuilder::createNew();
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
      default: return ao::library::TrackBuilder::createNew();
    }
  }
} // namespace ao::tag::mpeg::id3v2
