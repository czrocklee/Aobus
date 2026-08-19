// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "File.h"

#include "../detail/Content.h"
#include "../detail/Decoder.h"
#include "../detail/PictureBlock.h"
#include "../detail/VorbisComment.h"
#include <ao/AudioCodec.h>
#include <ao/Error.h>
#include <ao/media/file/File.h>
#include <ao/media/ogg/Demuxer.h>
#include <ao/media/opus/Header.h>
#include <ao/media/opus/Timeline.h>
#include <ao/utility/Base64.h>
#include <ao/utility/ByteView.h>

#include <chrono>
#include <cstddef>
#include <expected>
#include <span>
#include <string_view>
#include <utility>

namespace ao::media::file::opus
{
  using namespace media::opus;

  namespace
  {
    // Vorbis comment key carrying a Base64-encoded METADATA_BLOCK_PICTURE body.
    constexpr std::string_view kPictureKey = "METADATA_BLOCK_PICTURE";

    bool isPictureKey(std::string_view key) noexcept
    {
      if (key.size() != kPictureKey.size())
      {
        return false;
      }

      for (std::size_t index = 0; index < key.size(); ++index)
      {
        auto const character = key[index];
        auto const upper = character >= 'a' && character <= 'z' ? static_cast<char>(character - 'a' + 'A') : character;

        if (upper != kPictureKey[index])
        {
          return false;
        }
      }

      return true;
    }

    void appendPicture(detail::ContentBuilder& builder, std::string_view value)
    {
      auto const optDecoded = utility::base64Decode(value);

      if (!optDecoded)
      {
        return;
      }

      auto const decoded = decodeString(*optDecoded);
      auto const optBlock = detail::parsePictureBlock(utility::bytes::view(decoded));

      if (!optBlock || optBlock->bytes.empty())
      {
        return;
      }

      // Only a validated image needs cache-owned backing. The Base64 envelope
      // and picture-block fields are no longer needed after parsing.
      auto const stored = builder.own(decodeString(optBlock->bytes));
      builder.coverArt().add(optBlock->type, utility::bytes::view(stored));
    }

    void appendTags(detail::ContentBuilder& builder, std::span<std::byte const> body)
    {
      // RFC 7845 permits padding and further data after the comment list.
      auto const optComments = detail::parseVorbisComments(body, detail::VorbisCommentTrailing::Allowed);

      if (!optComments)
      {
        return;
      }

      for (auto const comment : *optComments)
      {
        auto const optField = detail::splitVorbisComment(comment);

        if (!optField)
        {
          continue;
        }

        if (!detail::applyVorbisComment(builder, *optField) && isPictureKey(optField->key))
        {
          appendPicture(builder, optField->value);
        }
      }
    }

    std::chrono::milliseconds streamDuration(Timeline const& timeline) noexcept
    {
      // A stream that never declared a complete ending reports no duration
      // rather than a guess; a stream that declared zero audible frames does.
      if (!timeline.optTotalFrames)
      {
        return std::chrono::milliseconds{0};
      }

      return std::chrono::milliseconds{(*timeline.optTotalFrames * std::chrono::milliseconds::period::den) /
                                       kDecodedSampleRate};
    }
  } // namespace

  Result<File::Index> File::parseIndex() const
  {
    auto demuxerRes = media::ogg::Demuxer::parse(bytes());

    if (!demuxerRes)
    {
      return std::unexpected{demuxerRes.error()};
    }

    if (demuxerRes->packetCount() <= kTagsPacketIndex)
    {
      return makeError(Error::Code::CorruptData, "ogg file carries no opus header packets");
    }

    auto headRes = parseHead(demuxerRes->packet(kHeadPacketIndex).bytes);

    if (!headRes)
    {
      return std::unexpected{headRes.error()};
    }

    if (demuxerRes->packetCount() <= kFirstAudioPacketIndex)
    {
      return makeError(Error::Code::CorruptData, "opus file has no audio payload");
    }

    auto timelineRes = deriveOggTimeline(*demuxerRes, *headRes);

    if (!timelineRes)
    {
      return std::unexpected{timelineRes.error()};
    }

    // Identity starts at the first audio page. Retagging preserves it unless a
    // changed header-page count renumbers the audio pages retained here.
    auto const payloadOffset = demuxerRes->packetPageOffset(kFirstAudioPacketIndex);
    return Index{.demuxer = std::move(*demuxerRes),
                 .head = *headRes,
                 .timeline = *timelineRes,
                 .payload = payloadRange(payloadOffset, bytes().size() - payloadOffset)};
  }

  Result<File::Index> const& File::index() const
  {
    if (!_optIndexResult)
    {
      _optIndexResult.emplace(parseIndex());
    }

    return *_optIndexResult;
  }

  Result<detail::Content> File::readContent() const
  {
    auto const& indexResult = index();

    if (!indexResult)
    {
      return std::unexpected{indexResult.error()};
    }

    auto const& head = indexResult->head;
    auto builder = detail::ContentBuilder::makeEmpty();

    // Opus decodes at a fixed rate, so the encoder's input rate never describes
    // the decoded signal and the format carries no sample depth to report.
    builder.property()
      .sampleRate(SampleRate{kDecodedSampleRate})
      .channels(Channels{head.channels})
      .codec(AudioCodec::Opus);

    if (auto const duration = streamDuration(indexResult->timeline); duration > std::chrono::milliseconds{0})
    {
      builder.property().duration(duration).bitrate(Bitrate{bitrateFromBytes(bytes().size(), duration)});
    }

    if (auto const optBody = parseTagsBody(indexResult->demuxer.packet(kTagsPacketIndex).bytes); optBody)
    {
      appendTags(builder, *optBody);
    }

    return std::move(builder).finish();
  }

  Result<PayloadView> File::audioPayload() const
  {
    auto const& indexResult = index();

    if (!indexResult)
    {
      return std::unexpected{indexResult.error()};
    }

    return indexResult->payload;
  }
} // namespace ao::media::file::opus
