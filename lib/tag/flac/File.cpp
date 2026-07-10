// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "File.h"

#include "../detail/Decoder.h"
#include <ao/AudioCodec.h>
#include <ao/Error.h>
#include <ao/library/CoverArt.h>
#include <ao/library/TrackBuilder.h>
#include <ao/media/detail/MediaError.h>
#include <ao/media/flac/MetadataBlock.h>
#include <ao/media/flac/MetadataBlockLayout.h>
#include <ao/tag/TagFile.h>
#include <ao/tag/detail/TagError.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <expected>
#include <string_view>

namespace ao::tag::flac
{
  using namespace media::flac;

  namespace
  {
    using TextSetter =
      library::TrackBuilder::MetadataBuilder& (library::TrackBuilder::MetadataBuilder::*)(std::string_view);
    using NumberSetter =
      library::TrackBuilder::MetadataBuilder& (library::TrackBuilder::MetadataBuilder::*)(std::uint16_t);

    template<TextSetter Setter>
    void handleText(library::TrackBuilder& builder, std::string_view value)
    {
      (builder.metadata().*Setter)(value);
    }

    [[maybe_unused]] void handleEnsembleFallback(library::TrackBuilder& builder, std::string_view value)
    {
      if (builder.metadata().ensemble().empty())
      {
        builder.metadata().ensemble(value);
      }
    }

    [[maybe_unused]] void handleSoloistFallback(library::TrackBuilder& builder, std::string_view value)
    {
      if (builder.metadata().soloist().empty())
      {
        builder.metadata().soloist(value);
      }
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
      auto const pair = parseSlashPair(value);

      if (pair.optPrimary)
      {
        (builder.metadata().*PrimarySetter)(*pair.optPrimary);
      }

      if (pair.optSecondary)
      {
        (builder.metadata().*SecondarySetter)(*pair.optSecondary);
      }
    }

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4267) // gperf's generated hash narrows size_t lengths
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#endif
#include "tag/flac/VorbisCommentDispatch.h"
#ifdef _MSC_VER
#pragma warning(pop)
#else
#pragma GCC diagnostic pop
#endif
  } // namespace

  Result<library::TrackBuilder> File::loadTrackImpl() const
  {
    if (size() < 4 || std::memcmp(address(), "fLaC", 4) != 0)
    {
      return makeError(Error::Code::CorruptData, "unrecognized flac file content");
    }

    try
    {
      clearOwnedStrings();
      auto builder = library::TrackBuilder::makeEmpty();

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
                builder.property().duration(duration).bitrate(Bitrate{bitrateFromBytes(size(), duration)});
              }
            }

            break;
          }

          case MetadataBlockType::VorbisComment:
          {
            VorbisCommentBlockView{iter->data()}.visitComments(
              [&](std::string_view comment)
              {
                if (auto const equalsOffset = comment.find('='); equalsOffset != std::string_view::npos)
                {
                  auto const key = comment.substr(0, equalsOffset);

                  if (auto const value = comment.substr(equalsOffset + 1);
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
    catch (detail::TagException const& ex)
    {
      return std::unexpected{ex.error()};
    }
    catch (media::detail::MediaException const& ex)
    {
      return std::unexpected{ex.error()};
    }
  }

  Result<AudioPayload> File::audioPayloadImpl() const
  {
    if (size() < 4 || std::memcmp(address(), "fLaC", 4) != 0)
    {
      return makeError(Error::Code::CorruptData, "unrecognized flac file content");
    }

    try
    {
      std::size_t offset = 4;
      auto iter = MetadataBlockViewIterator{static_cast<char const*>(address()) + offset, size() - offset};
      auto const end = MetadataBlockViewIterator{};

      for (; iter != end; ++iter)
      {
        auto const blockSize = iter->size();

        if (blockSize > size() - offset)
        {
          return makeError(Error::Code::CorruptData, "invalid flac metadata blocks size, exceeding the file boundary");
        }

        offset += blockSize;

        if (iter->layout<MetadataBlockLayout>().isLastBlock)
        {
          if (offset == size())
          {
            return makeError(Error::Code::CorruptData, "flac file has no audio payload");
          }

          return payloadRange(offset, size() - offset);
        }
      }

      return makeError(Error::Code::CorruptData, "invalid flac metadata blocks, missing last metadata block");
    }
    catch (media::detail::MediaException const& ex)
    {
      return std::unexpected{ex.error()};
    }
  }
} // namespace ao::tag::flac
