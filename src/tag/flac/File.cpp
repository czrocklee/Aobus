// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "../Decoder.h"
#include "MetadataBlock.h"
#include <rs/Exception.h>
#include <rs/tag/flac/File.h>
#include <rs/utility/ByteView.h>

#include <cstring>
#include <string>
#include <string_view>

namespace rs::tag::flac
{
  namespace
  {
    using TextSetter =
      rs::core::TrackBuilder::MetadataBuilder& (rs::core::TrackBuilder::MetadataBuilder::*)(std::string_view);
    using NumberSetter =
      rs::core::TrackBuilder::MetadataBuilder& (rs::core::TrackBuilder::MetadataBuilder::*)(std::uint16_t);

    template<TextSetter Setter>
    void handleText(rs::core::TrackBuilder& builder, std::string_view value)
    {
      (builder.metadata().*Setter)(value);
    }

    template<NumberSetter Setter>
    void handleNumber(rs::core::TrackBuilder& builder, std::string_view value)
    {
      if (auto parsed = decodeUint16(value); parsed)
      {
        (builder.metadata().*Setter)(*parsed);
      }
    }

    template<NumberSetter PrimarySetter, NumberSetter SecondarySetter>
    void handleSlashNumber(rs::core::TrackBuilder& builder, std::string_view value)
    {
      auto const separator = value.find('/');
      handleNumber<PrimarySetter>(builder, value.substr(0, separator));
      if (separator != std::string_view::npos)
      {
        handleNumber<SecondarySetter>(builder, value.substr(separator + 1));
      }
    }

#include "tag/flac/VorbisCommentDispatch.h"

  } // namespace

  rs::core::TrackBuilder File::loadTrack() const
  {
    if (_mappedRegion.get_size() < 4 || std::memcmp(_mappedRegion.get_address(), "fLaC", 4) != 0)
    {
      RS_THROW(rs::Exception, "unrecognized flac file content");
    }

    clearOwnedStrings();
    auto builder = rs::core::TrackBuilder::createNew();

    auto iter = MetadataBlockViewIterator{
      static_cast<char const*>(_mappedRegion.get_address()) + 4, _mappedRegion.get_size() - 4};
    auto end = MetadataBlockViewIterator{};

    for (; iter != end; ++iter)
    {
      switch (iter->type())
      {
        case MetadataBlockType::StreamInfo:
        {
          auto view = StreamInfoBlockView{iter->data()};
          builder.property().sampleRate(view.sampleRate()).channels(view.channels()).bitDepth(view.bitDepth());

          if (auto const totalSamples = view.totalSamples(); view.sampleRate() > 0 && totalSamples > 0)
          {
            auto const durationMs = static_cast<std::uint32_t>((totalSamples * 1000) / view.sampleRate());
            if (durationMs > 0)
            {
              builder.property()
                .durationMs(durationMs)
                .bitrate(static_cast<std::uint32_t>((_mappedRegion.get_size() * 8000) / durationMs));
            }
          }

          break;
        }

        case MetadataBlockType::VorbisComment:
        {
          VorbisCommentBlockView{iter->data()}.visitComments([&](std::string_view comment)
          {
            auto const pos = comment.find('=');
            if (pos == std::string_view::npos)
            {
              return;
            }

            std::string_view key = comment.substr(0, pos);
            std::string_view value = comment.substr(pos + 1);

            if (auto const* entry = FlacVorbisDispatchTable::lookupVorbisField(key.data(), key.size()))
            {
              entry->handler(builder, value);
            }
            else
            {
              builder.custom().add(key, value);
            }
          });

          break;
        }

        case MetadataBlockType::Picture:
        {
          builder.metadata().coverArtData(PictureBlockView{iter->data()}.blob());
          break;
        }

        default: break;
      }
    }

    return builder;
  }
} // namespace rs::tag::flac
