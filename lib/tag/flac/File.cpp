// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "File.h"

#include "../detail/Decoder.h"
#include <ao/AudioCodec.h>
#include <ao/Exception.h>
#include <ao/library/CoverArt.h>
#include <ao/library/TrackBuilder.h>
#include <ao/media/flac/MetadataBlock.h>
#include <ao/media/flac/MetadataBlockLayout.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace ao::tag::flac
{
  using namespace media::flac;

  namespace
  {
    // Bits per byte for bitrate calculation
    constexpr std::uint32_t kBitsPerByte = 8;

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

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#include "tag/flac/VorbisCommentDispatch.h"
#pragma GCC diagnostic pop
  } // namespace

  library::TrackBuilder File::loadTrack() const
  {
    if (size() < 4 || std::memcmp(address(), "fLaC", 4) != 0)
    {
      ao::throwException<Exception>("unrecognized flac file content");
    }

    clearOwnedStrings();
    auto builder = library::TrackBuilder::createNew();

    auto iter = MetadataBlockViewIterator{static_cast<char const*>(address()) + 4, size() - 4};
    auto const end = MetadataBlockViewIterator{};

    for (; iter != end; ++iter)
    {
      switch (iter->type())
      {
        case MetadataBlockType::StreamInfo:
        {
          auto view = StreamInfoBlockView{iter->data()};
          builder.property()
            .sampleRate(SampleRate{view.sampleRate()})
            .channels(Channels{view.channels()})
            .bitDepth(BitDepth{view.bitDepth()})
            .codec(AudioCodec::Flac);

          if (auto const totalSamples = view.totalSamples(); view.sampleRate() > 0 && totalSamples > 0)
          {
            if (auto const duration =
                  std::chrono::milliseconds{(totalSamples * std::chrono::milliseconds::period::den) /
                                            view.sampleRate()};
                duration > std::chrono::milliseconds{0})
            {
              builder.property().duration(duration).bitrate(Bitrate{static_cast<std::uint32_t>(
                (size() * kBitsPerByte * std::chrono::milliseconds::period::den) / duration.count())});
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

                if (auto const value = comment.substr(pos + 1);
                    auto const* entry = FlacVorbisDispatchTable::lookupVorbisField(key.data(), key.size()))
                {
                  entry->handler(builder, value);
                }
              }
            });

          break;
        }

        case MetadataBlockType::Picture:
        {
          auto const pic = PictureBlockView{iter->data()};
          auto const rawType = pic.pictureType();
          auto const picType = rawType <= static_cast<std::uint32_t>(library::PictureType::PublisherLogo)
                                 ? static_cast<library::PictureType>(rawType)
                                 : library::PictureType::Other;
          builder.coverArt().add(picType, pic.blob());
          break;
        }

        default: break;
      }
    }

    return builder;
  }
} // namespace ao::tag::flac
