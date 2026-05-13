// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "File.h"
#include "../detail/Decoder.h"
#include <ao/Exception.h>
#include <ao/media/flac/MetadataBlock.h>
#include <ao/utility/ByteView.h>

#include <cstring>
#include <string>
#include <string_view>

namespace ao::tag::flac
{
  using namespace media::flac;

  namespace
  {
    // Bits per byte for bitrate calculation
    constexpr std::uint32_t kBitsPerByte = 8;
    // Milliseconds per second
    constexpr std::uint32_t kMsPerSecond = 1000;

    using TextSetter =
      library::TrackBuilder::MetadataBuilder& (library::TrackBuilder::MetadataBuilder::*)(std::string_view);
    using NumberSetter =
      library::TrackBuilder::MetadataBuilder& (library::TrackBuilder::MetadataBuilder::*)(std::uint16_t);

    template<TextSetter Setter>
    void handleText(library::TrackBuilder& builder, std::string_view value)
    {
      (builder.metadata().*Setter)(value);
    }

    template<NumberSetter Setter>
    void handleNumber(library::TrackBuilder& builder, std::string_view value)
    {
      if (auto const optParsed = decodeUint16(value); optParsed)
      {
        (builder.metadata().*Setter)(*optParsed);
      }
    }

    template<NumberSetter PrimarySetter, NumberSetter SecondarySetter>
    void handleSlashNumber(library::TrackBuilder& builder, std::string_view value)
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

  library::TrackBuilder File::loadTrack() const
  {
    if (_mappedRegion.get_size() < 4 || std::memcmp(_mappedRegion.get_address(), "fLaC", 4) != 0)
    {
      AO_THROW(Exception, "unrecognized flac file content");
    }

    clearOwnedStrings();
    auto builder = library::TrackBuilder::createNew();

    auto iter = MetadataBlockViewIterator{
      static_cast<char const*>(_mappedRegion.get_address()) + 4, _mappedRegion.get_size() - 4};
    auto const end = MetadataBlockViewIterator{};

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
            if (auto const durationMs = static_cast<std::uint32_t>((totalSamples * kMsPerSecond) / view.sampleRate());
                durationMs > 0)
            {
              builder.property()
                .durationMs(durationMs)
                .bitrate(
                  static_cast<std::uint32_t>((_mappedRegion.get_size() * kBitsPerByte * kMsPerSecond) / durationMs));
            }
          }

          break;
        }

        case MetadataBlockType::VorbisComment:
        {
          VorbisCommentBlockView{iter->data()}.visitComments(
            [&](std::string_view comment)
            {
              if (auto const pos = comment.find('='); pos != std::string_view::npos)
              {
                auto const key = comment.substr(0, pos);
                auto const value = comment.substr(pos + 1);

                if (auto const* entry = FlacVorbisDispatchTable::lookupVorbisField(key.data(), key.size()))
                {
                  entry->handler(builder, value);
                }
                else
                {
                  builder.custom().add(key, value);
                }
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
} // namespace ao::tag::flac
