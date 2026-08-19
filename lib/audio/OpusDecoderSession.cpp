// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "OpusDecoderSession.h"

#include "AudioTime.h"
#include "detail/DecoderError.h"
#include "detail/DecoderOutputAdapter.h"
#include "detail/OggPacketSource.h"
#include <ao/AudioCodec.h>
#include <ao/Contract.h>
#include <ao/Error.h>
#include <ao/audio/DecodedStreamInfo.h>
#include <ao/audio/PcmBlock.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>
#include <ao/media/opus/Header.h>
#include <ao/media/opus/Timeline.h>

#include <opus/opus_defines.h>
#include <opus/opus_multistream.h>
#include <opus/opus_types.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ao::audio
{
  namespace
  {
    // Opus decodes into 16-bit PCM here for the same reason the other lossy
    // decoders do: the PCM adapter cannot convert floating-point output to the
    // integer formats every exclusive-mode device exposes.
    constexpr std::uint8_t kOpusPcmBitDepth = 16;

    // Mapping family 1 orders its channels the way Vorbis does, while every
    // Aobus PCM surface is in WAV speaker order. Each row lists, for one WAV
    // output channel, the family-1 slot that feeds it. Families 0 and 255 need
    // no permutation: family 0 is mono or plain stereo, and family 255 defines
    // no speaker positions at all.
    constexpr auto kSurroundToWavOrder = std::to_array<std::array<std::uint8_t, media::opus::kMaxSurroundChannels>>({
      {0},                      // mono
      {0, 1},                   // L R
      {0, 2, 1},                // L C R -> L R C
      {0, 1, 2, 3},             // quad is already in WAV order
      {0, 2, 1, 3, 4},          // 5.0
      {0, 2, 1, 5, 3, 4},       // 5.1
      {0, 2, 1, 6, 5, 3, 4},    // 6.1
      {0, 2, 1, 7, 5, 6, 3, 4}, // 7.1
    });

    // Returns the channel mapping to hand libopus so its output lands in WAV
    // speaker order. The parsed Head keeps the file's own order untouched.
    std::array<std::uint8_t, media::opus::kMaxChannels> decoderChannelMapping(media::opus::Head const& head)
    {
      auto mapping = head.channelMapping;

      if (head.channelMappingFamily != media::opus::kMappingFamilySurround)
      {
        return mapping;
      }

      AO_EXPECTS(head.channels >= 1 && head.channels <= media::opus::kMaxSurroundChannels);
      auto const& order = kSurroundToWavOrder.at(head.channels - 1U);

      for (std::uint8_t channel = 0; channel < head.channels; ++channel)
      {
        mapping[channel] = head.channelMapping[order.at(channel)];
      }

      return mapping;
    }

    std::string opusErrorMessage(std::int32_t error)
    {
      auto const* const detail = ::opus_strerror(error);
      return detail != nullptr ? std::string{detail} : std::string{"Unknown Opus error"};
    }
  } // namespace

  struct OpusDecoderSession::Impl final
  {
    detail::DecoderOutputAdapter outputAdapter;
    DecodedStreamInfo info;
    detail::OggPacketSource packetSource;
    media::opus::Head head;
    media::opus::Timeline timeline;
    ::OpusMSDecoder* decoderHandle = nullptr;
    std::vector<opus_int16> pcmBuffer;
    // Decoded samples still to drop from the front, covering both the header
    // pre-skip and the distance between a seek restart point and its target.
    std::int64_t pendingDiscard = 0;
    // Audible frames already handed out, which is also the next block's index.
    std::int64_t emittedFrames = 0;

    explicit Impl(std::optional<SampleEncoding> optOutputEncoding)
      : outputAdapter{optOutputEncoding}
    {
    }

    ~Impl()
    {
      if (decoderHandle != nullptr)
      {
        ::opus_multistream_decoder_destroy(decoderHandle);
      }
    }

    Impl(Impl const&) = delete;
    Impl& operator=(Impl const&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    void openDecoder()
    {
      std::int32_t error = OPUS_OK;
      auto const mapping = decoderChannelMapping(head);
      decoderHandle = ::opus_multistream_decoder_create(static_cast<opus_int32>(media::opus::kDecodedSampleRate),
                                                        head.channels,
                                                        head.streamCount,
                                                        head.coupledStreamCount,
                                                        mapping.data(),
                                                        &error);

      if (decoderHandle == nullptr || error != OPUS_OK)
      {
        detail::throwDecoderError(
          Error::Code::InitFailed, std::format("Failed to create Opus decoder: {}", opusErrorMessage(error)));
      }

      // OpusHead carries its output gain in the same Q7.8 decibel unit that
      // OPUS_SET_GAIN takes, so the decoder applies RFC 7845 header gain itself.
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) libopus exposes decoder control as a vararg C entry point.
      if (auto const result = ::opus_multistream_decoder_ctl(decoderHandle, OPUS_SET_GAIN(head.outputGain));
          result != OPUS_OK)
      {
        detail::throwDecoderError(
          Error::Code::InitFailed, std::format("Failed to apply Opus header gain: {}", opusErrorMessage(result)));
      }

      pcmBuffer.resize(static_cast<std::size_t>(media::opus::kMaxPacketFrames) * head.channels);
    }

    void configureStreamInfo()
    {
      info.sourceFormat = SignalFormat{.sampleRate = media::opus::kDecodedSampleRate,
                                       .channels = head.channels,
                                       .precisionBits = kOpusPcmBitDepth,
                                       .sampleKind = SampleKind::Integer};
      info.isLossy = true;
      info.codec = AudioCodec::Opus;

      auto const configuredRes = outputAdapter.configure(info.sourceFormat, SampleEncoding::Signed16Le);

      if (!configuredRes)
      {
        detail::throwDecoderError(configuredRes.error());
      }

      info.outputFormat = *configuredRes;

      if (timeline.optTotalFrames)
      {
        info.duration = samplesToDuration(*timeline.optTotalFrames, media::opus::kDecodedSampleRate);
      }
    }

    void resetDecoder() const noexcept
    {
      if (decoderHandle != nullptr)
      {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) libopus exposes decoder control as a vararg C entry point.
        ::opus_multistream_decoder_ctl(decoderHandle, OPUS_RESET_STATE);
      }
    }

    // Decodes one packet into pcmBuffer and reports the frames it produced.
    std::int32_t decodeCurrentPacket()
    {
      auto const packet = packetSource.packet();

      if (packet.empty())
      {
        detail::throwDecoderError(Error::Code::DecodeFailed, "Failed to read Opus packet payload");
      }

      auto const decoded = ::opus_multistream_decode(decoderHandle,
                                                     // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
                                                     reinterpret_cast<unsigned char const*>(packet.data()),
                                                     static_cast<opus_int32>(packet.size()),
                                                     pcmBuffer.data(),
                                                     media::opus::kMaxPacketFrames,
                                                     0);

      if (decoded < 0)
      {
        detail::throwDecoderError(
          Error::Code::DecodeFailed, std::format("Opus decode failed: {}", opusErrorMessage(decoded)));
      }

      packetSource.advance();
      return decoded;
    }

    bool isExhausted() const noexcept
    {
      return packetSource.isAtEnd() ||
             (timeline.optTotalFrames && std::cmp_greater_equal(emittedFrames, *timeline.optTotalFrames));
    }
  };

  OpusDecoderSession::OpusDecoderSession(std::optional<SampleEncoding> optOutputEncoding)
    : _implPtr{std::make_unique<Impl>(optOutputEncoding)}
  {
  }

  OpusDecoderSession::~OpusDecoderSession() = default;

  Result<> OpusDecoderSession::initialize(std::filesystem::path const& filePath) noexcept
  {
    try
    {
      if (auto const result = _implPtr->packetSource.open(filePath); !result)
      {
        detail::throwDecoderError(result.error());
      }

      if (_implPtr->packetSource.packetCount() <= media::opus::kFirstAudioPacketIndex)
      {
        detail::throwDecoderError(Error::Code::FormatRejected, "Ogg stream carries no Opus audio packets");
      }

      auto headRes = media::opus::parseHead(_implPtr->packetSource.packetAt(media::opus::kHeadPacketIndex));

      if (!headRes)
      {
        detail::throwDecoderError(headRes.error());
      }

      _implPtr->head = *headRes;

      auto timelineRes = media::opus::deriveOggTimeline(_implPtr->packetSource.demuxer(), _implPtr->head);

      if (!timelineRes)
      {
        detail::throwDecoderError(timelineRes.error());
      }

      _implPtr->timeline = *timelineRes;
      _implPtr->openDecoder();
      _implPtr->configureStreamInfo();

      _implPtr->packetSource.setPacketIndex(media::opus::kFirstAudioPacketIndex);
      _implPtr->pendingDiscard = _implPtr->timeline.playbackStartGranule - _implPtr->timeline.decodeStartGranule;
      _implPtr->emittedFrames = 0;

      return {};
    }
    catch (detail::DecoderException const& ex)
    {
      return std::unexpected{ex.error()};
    }
  }

  // Result error materialization may allocate; DecoderSession intentionally
  // fails fast if an allocation escapes this noexcept boundary.
  Result<> OpusDecoderSession::seek(std::chrono::milliseconds offset) noexcept
  {
    if (!_implPtr->packetSource.isOpen())
    {
      return makeError(Error::Code::SeekFailed, "Opus packet source is not open");
    }

    // Playback positions are measured from the first audible sample, which sits
    // at the timeline origin advanced past the pre-skip.
    auto const requestedFrames = static_cast<std::int64_t>(
      durationToSamples(std::max(offset, std::chrono::milliseconds{0}), media::opus::kDecodedSampleRate));
    auto const targetGranule = _implPtr->timeline.playbackStartGranule + requestedFrames;
    // Resuming on the page group that merely contains the target leaves the
    // decoder cold for the first block it emits. Aiming the search a pre-roll
    // earlier walks back however many page groups that distance spans, and the
    // surplus is discarded rather than played, so the seek lands unchanged.
    auto const restartGranule = _implPtr->packetSource.seekToGranule(targetGranule - media::opus::kSeekPreRollFrames);

    // Restarting at or before the first audio packet resumes at the timeline
    // origin. The header pages carry the granule position zero RFC 7845 fixes
    // for them, which names no position on a stream that starts cropped.
    auto const resumeGranule = _implPtr->packetSource.packetIndex() <= media::opus::kFirstAudioPacketIndex
                                 ? _implPtr->timeline.decodeStartGranule
                                 : restartGranule;

    if (_implPtr->packetSource.packetIndex() < media::opus::kFirstAudioPacketIndex)
    {
      _implPtr->packetSource.setPacketIndex(media::opus::kFirstAudioPacketIndex);
    }

    _implPtr->pendingDiscard = std::max<std::int64_t>(targetGranule - resumeGranule, 0);
    _implPtr->emittedFrames = std::max<std::int64_t>(requestedFrames, 0);
    flush();
    return {};
  }

  void OpusDecoderSession::flush() noexcept
  {
    _implPtr->resetDecoder();
  }

  // Result error materialization and decode buffers may allocate; DecoderSession
  // intentionally fails fast if an allocation escapes this noexcept boundary.
  Result<PcmBlock> OpusDecoderSession::readNextBlock() noexcept
  {
    try
    {
      auto const channels = _implPtr->info.sourceFormat.channels;

      // A packet consumed entirely by the pre-skip or a seek discard produces no
      // audible frames, so keep decoding until one does.
      while (!_implPtr->isExhausted())
      {
        auto const decoded = _implPtr->decodeCurrentPacket();
        auto const dropped = std::min<std::int64_t>(_implPtr->pendingDiscard, decoded);
        _implPtr->pendingDiscard -= dropped;

        auto usable = decoded - dropped;

        if (_implPtr->timeline.optTotalFrames)
        {
          usable = std::min<std::int64_t>(
            usable, static_cast<std::int64_t>(*_implPtr->timeline.optTotalFrames) - _implPtr->emittedFrames);
        }

        if (usable <= 0)
        {
          continue;
        }

        auto const firstFrameIndex = static_cast<std::uint64_t>(_implPtr->emittedFrames);
        _implPtr->emittedFrames += usable;

        auto const offset = static_cast<std::size_t>(dropped) * channels;
        auto const count = static_cast<std::size_t>(usable) * channels;
        auto const nativeBytes = std::as_bytes(std::span{_implPtr->pcmBuffer}.subspan(offset, count));
        auto convertedRes = _implPtr->outputAdapter.convert(nativeBytes);

        if (!convertedRes)
        {
          detail::throwDecoderError(convertedRes.error());
        }

        return PcmBlock{
          .bytes = *convertedRes,
          .frames = static_cast<std::uint32_t>(usable),
          .firstFrameIndex = firstFrameIndex,
          .endOfStream = _implPtr->isExhausted(),
        };
      }

      return PcmBlock{.endOfStream = true};
    }
    catch (detail::DecoderException const& ex)
    {
      return std::unexpected{ex.error()};
    }
  }

  DecodedStreamInfo OpusDecoderSession::streamInfo() const noexcept
  {
    return _implPtr->info;
  }
} // namespace ao::audio
