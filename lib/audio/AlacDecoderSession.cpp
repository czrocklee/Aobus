// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "AlacDecoderSession.h"

#include "detail/DecoderError.h"
#include "detail/DecoderOutputAdapter.h"
#include "detail/Mp4PacketSource.h"
#include <ao/AudioCodec.h>
#include <ao/Error.h>
#include <ao/audio/ChannelLayoutEvidence.h>
#include <ao/audio/DecodedStreamInfo.h>
#include <ao/audio/PcmBlock.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>
#include <ao/media/mp4/Atom.h>
#include <ao/utility/ByteView.h>

#include <alac/ALACAudioTypes.h>
#include <alac/ALACBitUtilities.h>
#include <alac/ALACDecoder.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::audio
{
  using namespace utility;

  namespace
  {
    constexpr std::size_t kAlacConfigOffset = 12;
    constexpr std::size_t kAlacConfigSize = 24;
    constexpr std::size_t kAlacCompatibleVersionOffset = kAlacConfigOffset + 4;
    constexpr std::size_t kAlacBitDepthOffset = kAlacConfigOffset + 5;
    constexpr std::size_t kAlacChannelCountOffset = kAlacConfigOffset + 9;
    constexpr std::size_t kAlacSampleRateOffset = kAlacConfigOffset + 20;
    constexpr std::uint32_t kMaxAlacFrameLength = 16384;
    constexpr std::uint32_t kMaxAlacSampleRate = 384000;
    constexpr std::uint8_t kMaxAlacChannels = 8;
    constexpr std::uint8_t kLastWaveChannelLabel = 18;
    constexpr std::uint32_t kAmbiguousThreeChannelTag = 0x00710003;
    constexpr std::uint32_t kChannelBitmapTag = 0x00010000;
    constexpr std::uint32_t kLayoutTagChannelCountMask = 0xFFFF;
    constexpr std::size_t kAlacCookiePayloadPrefixBytes = sizeof(std::uint32_t) + kAlacConfigSize;
    constexpr std::size_t kAudioSampleEntryHeaderBytes = 28;
    constexpr std::size_t kAudioSampleEntryVersionOffset = 8;
    constexpr std::size_t kBigEndian32Byte1Offset = 1;
    constexpr std::size_t kBigEndian32Byte2Offset = 2;
    constexpr std::size_t kBigEndian32Byte3Offset = 3;

    std::uint32_t readBigEndian32(std::span<std::byte const> bytes, std::size_t offset) noexcept
    {
      return (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
             (static_cast<std::uint32_t>(bytes[offset + kBigEndian32Byte1Offset]) << 16U) |
             (static_cast<std::uint32_t>(bytes[offset + kBigEndian32Byte2Offset]) << 8U) |
             static_cast<std::uint32_t>(bytes[offset + kBigEndian32Byte3Offset]);
    }

    void validateAlacCookie(std::span<std::byte const> cookie)
    {
      if (cookie.size() < kAlacConfigOffset + kAlacConfigSize ||
          utility::bytes::stringView(cookie.subspan(4, 4)) != "alac")
      {
        detail::throwDecoderError(Error::Code::InitFailed, "Malformed ALAC configuration");
      }

      auto const frameLength = readBigEndian32(cookie, kAlacConfigOffset);
      auto const compatibleVersion = static_cast<std::uint8_t>(cookie[kAlacCompatibleVersionOffset]);
      auto const bitDepth = static_cast<std::uint8_t>(cookie[kAlacBitDepthOffset]);
      auto const channels = static_cast<std::uint8_t>(cookie[kAlacChannelCountOffset]);
      auto const sampleRate = readBigEndian32(cookie, kAlacSampleRateOffset);
      auto const supportedBitDepth = bitDepth == 16 || bitDepth == 20 || bitDepth == 24 || bitDepth == 32;

      if (frameLength == 0 || frameLength > kMaxAlacFrameLength || compatibleVersion != 0 || !supportedBitDepth ||
          channels == 0 || channels > kMaxAlacChannels || sampleRate == 0 || sampleRate > kMaxAlacSampleRate)
      {
        detail::throwDecoderError(Error::Code::InitFailed, "Invalid ALAC stream configuration");
      }
    }

    struct AlacChannelLayout final
    {
      std::uint32_t speakerMask = 0;
      std::array<std::uint8_t, kMaxAlacChannels> sourceSlots{};

      bool operator==(AlacChannelLayout const&) const = default;
    };

    struct AlacNamedLayout final
    {
      std::uint32_t tag;
      std::array<std::uint8_t, kMaxAlacChannels> labels;
      bool isAlacCookieTag;
    };

    // Full tag values, not channel-count guesses. Core Audio labels 1..18
    // correspond to the assigned WAVE bits; rear-surround labels 33/34 do not.
    // Arrival-order names follow Apple's CoreAudioBaseTypes.h.
    constexpr auto kAlacNamedLayouts = std::to_array<AlacNamedLayout>({
      // Mono: C.
      {.tag = 0x00640001, .labels = {3}, .isAlacCookieTag = true},
      // Stereo: L R.
      {.tag = 0x00650002, .labels = {1, 2}, .isAlacCookieTag = true},
      // MPEG 3.0 B: C L R.
      {.tag = 0x00720003, .labels = {3, 1, 2}, .isAlacCookieTag = false},
      // Quadraphonic: L R Ls Rs.
      {.tag = 0x006C0004, .labels = {1, 2, 5, 6}, .isAlacCookieTag = false},
      // MPEG 4.0 A: L R C Cs.
      {.tag = 0x00730004, .labels = {1, 2, 3, 9}, .isAlacCookieTag = false},
      // MPEG 4.0 B: C L R Cs.
      {.tag = 0x00740004, .labels = {3, 1, 2, 9}, .isAlacCookieTag = true},
      // MPEG 5.0 A: L R C Ls Rs.
      {.tag = 0x00750005, .labels = {1, 2, 3, 5, 6}, .isAlacCookieTag = false},
      // MPEG 5.0 B: L R Ls Rs C.
      {.tag = 0x00760005, .labels = {1, 2, 5, 6, 3}, .isAlacCookieTag = false},
      // MPEG 5.0 D: C L R Ls Rs.
      {.tag = 0x00780005, .labels = {3, 1, 2, 5, 6}, .isAlacCookieTag = true},
      // MPEG 5.1 A: L R C LFE Ls Rs.
      {.tag = 0x00790006, .labels = {1, 2, 3, 4, 5, 6}, .isAlacCookieTag = false},
      // MPEG 5.1 B: L R Ls Rs C LFE.
      {.tag = 0x007A0006, .labels = {1, 2, 5, 6, 3, 4}, .isAlacCookieTag = false},
      // MPEG 5.1 C: L C R Ls Rs LFE.
      {.tag = 0x007B0006, .labels = {1, 3, 2, 5, 6, 4}, .isAlacCookieTag = false},
      // MPEG 5.1 D: C L R Ls Rs LFE.
      {.tag = 0x007C0006, .labels = {3, 1, 2, 5, 6, 4}, .isAlacCookieTag = true},
      // AAC 6.1: C L R Ls Rs Cs LFE.
      {.tag = 0x008E0007, .labels = {3, 1, 2, 5, 6, 9, 4}, .isAlacCookieTag = true},
      // MPEG 7.1 A: L R C LFE Ls Rs Lc Rc.
      {.tag = 0x007E0008, .labels = {1, 2, 3, 4, 5, 6, 7, 8}, .isAlacCookieTag = false},
      // MPEG 7.1 B: C Lc Rc L R Ls Rs LFE.
      {.tag = 0x007F0008, .labels = {3, 7, 8, 1, 2, 5, 6, 4}, .isAlacCookieTag = true},
    });

    AlacChannelLayout channelLayoutForLabels(std::span<std::uint8_t const> labels) noexcept
    {
      auto layout = AlacChannelLayout{};

      if (labels.empty() || labels.size() > layout.sourceSlots.size())
      {
        return {};
      }

      for (auto const label : labels)
      {
        if (label == 0 || label > kLastWaveChannelLabel)
        {
          return {};
        }

        auto const bit = std::uint32_t{1} << (label - 1U);

        if ((layout.speakerMask & bit) != 0U)
        {
          return {};
        }

        layout.speakerMask |= bit;
      }

      std::size_t destination = 0;

      for (std::uint8_t label = 1; label <= kLastWaveChannelLabel; ++label)
      {
        for (std::size_t source = 0; source < labels.size(); ++source)
        {
          if (labels[source] == label)
          {
            layout.sourceSlots[destination++] = static_cast<std::uint8_t>(source);
          }
        }
      }

      return layout;
    }

    AlacChannelLayout channelLayoutForDescriptions(std::span<std::byte const> descriptions,
                                                   std::uint8_t channels) noexcept
    {
      constexpr std::size_t kDescriptionBytes = 20;

      if (descriptions.size() / kDescriptionBytes != channels || descriptions.size() % kDescriptionBytes != 0)
      {
        return {};
      }

      auto labels = std::array<std::uint8_t, kMaxAlacChannels>{};

      for (std::size_t index = 0; index < channels; ++index)
      {
        auto const offset = index * kDescriptionBytes;
        auto const label = readBigEndian32(descriptions, offset);
        auto const flags = readBigEndian32(descriptions, offset + 4U);

        // Named labels are authoritative; coordinates are ignored for them.
        // Coordinate-only, discrete and flagged descriptions are unsupported.
        if (label == 0 || label > kLastWaveChannelLabel || flags != 0)
        {
          return {};
        }

        labels[index] = static_cast<std::uint8_t>(label);
      }

      return channelLayoutForLabels(std::span<std::uint8_t const>{labels}.first(channels));
    }

    AlacChannelLayout channelLayoutForAtom(media::mp4::AtomView const& atom,
                                           std::uint8_t channels,
                                           bool alacCookieGrammar) noexcept
    {
      auto const payload = atom.payload();

      if (payload.size() < 16 || readBigEndian32(payload, 0) != 0 || readBigEndian32(atom.bytes(), 0) == 0)
      {
        return {};
      }

      auto const tag = readBigEndian32(payload, 4);
      auto const bitmap = readBigEndian32(payload, 8);
      auto const descriptionCount = readBigEndian32(payload, 12);

      // ALAC's cookie defines only named tags and two zero reserved words.
      // A sibling QuickTime chan may instead carry a bitmap or descriptions.
      if (alacCookieGrammar && (atom.bytes().size() != 24 || bitmap != 0 || descriptionCount != 0))
      {
        return {};
      }

      // The exact full value means C,L,R in ALAC, but L,R,C in generic
      // Core Audio. Physical sibling placement does not resolve that collision.
      if (tag == kAmbiguousThreeChannelTag)
      {
        return {};
      }

      if (!alacCookieGrammar && tag == kChannelBitmapTag)
      {
        if (payload.size() != 16 || descriptionCount != 0 || !isCompleteWaveSpeakerMask(bitmap, channels))
        {
          return {};
        }

        auto layout = AlacChannelLayout{.speakerMask = bitmap};

        for (std::uint8_t index = 0; index < channels; ++index)
        {
          layout.sourceSlots[index] = index;
        }

        return layout;
      }

      if (!alacCookieGrammar && tag == 0)
      {
        return bitmap == 0 && descriptionCount == channels ? channelLayoutForDescriptions(payload.subspan(16), channels)
                                                           : AlacChannelLayout{};
      }

      if (payload.size() != 16 || bitmap != 0 || descriptionCount != 0 ||
          (tag & kLayoutTagChannelCountMask) != channels)
      {
        return {};
      }

      for (auto const& named : kAlacNamedLayouts)
      {
        if (named.tag == tag && (!alacCookieGrammar || named.isAlacCookieTag))
        {
          return channelLayoutForLabels(std::span<std::uint8_t const>{named.labels}.first(channels));
        }
      }

      return {};
    }

    bool tryObserveAlacLayout(media::mp4::AtomView const& atom,
                              std::uint8_t channels,
                              bool alacCookieGrammar,
                              std::optional<AlacChannelLayout>& optLayout) noexcept
    {
      auto const layout = channelLayoutForAtom(atom, channels, alacCookieGrammar);

      // Any bad declaration vetoes identity, even beside a valid cookie or sibling.
      if (layout.speakerMask == 0 || (optLayout && *optLayout != layout))
      {
        return false;
      }

      optLayout = layout;
      return true;
    }

    bool tryObserveAlacCookieLayout(media::mp4::AtomView const& cookie,
                                    std::uint8_t channels,
                                    std::optional<AlacChannelLayout>& optLayout)
    {
      auto const payload = cookie.payload();

      if (readBigEndian32(cookie.bytes(), 0) == 1 || payload.size() < kAlacCookiePayloadPrefixBytes ||
          readBigEndian32(payload, 0) != 0)
      {
        return false;
      }

      auto cursor = media::mp4::AtomView::root(payload.subspan(kAlacCookiePayloadPrefixBytes)).children();

      while (true)
      {
        auto atomRes = cursor.next();

        if (!atomRes)
        {
          return false;
        }

        if (!*atomRes)
        {
          return true;
        }

        // The synthetic root only bounds an optional cookie suffix; it must
        // not grant that nested suffix a top-level size-to-end permission.
        if (auto const& atom = **atomRes;
            readBigEndian32(atom.bytes(), 0) == 0 ||
            (atom.type() == "chan" && !tryObserveAlacLayout(atom, channels, true, optLayout)))
        {
          return false;
        }
      }
    }

    AlacChannelLayout channelLayoutForEntry(media::mp4::OptionalAtom const& optEntry, std::uint8_t channels)
    {
      if (channels == 0 || channels > kMaxAlacChannels || !optEntry ||
          optEntry->payload().size() < kAudioSampleEntryHeaderBytes ||
          optEntry->payload()[kAudioSampleEntryVersionOffset] != std::byte{} ||
          optEntry->payload()[kAudioSampleEntryVersionOffset + 1U] != std::byte{})
      {
        return {};
      }

      auto optLayout = std::optional<AlacChannelLayout>{};
      auto cursor = optEntry->children();
      bool sawCookie = false;

      while (true)
      {
        auto atomRes = cursor.next();

        if (!atomRes)
        {
          return {};
        }

        if (!*atomRes)
        {
          break;
        }

        if (auto const& atom = **atomRes; atom.type() == "alac")
        {
          if (sawCookie || !tryObserveAlacCookieLayout(atom, channels, optLayout))
          {
            return {};
          }

          sawCookie = true;
        }
        else if (atom.type() == "chan" && !tryObserveAlacLayout(atom, channels, false, optLayout))
        {
          return {};
        }
      }

      if (!sawCookie)
      {
        return {};
      }

      if (optLayout)
      {
        return *optLayout;
      }

      // The ALAC specification leaves absent multichannel roles unassigned.
      if (channels == 1)
      {
        constexpr auto kMono = std::to_array<std::uint8_t>({3});
        return channelLayoutForLabels(kMono);
      }

      if (channels == 2)
      {
        constexpr auto kStereo = std::to_array<std::uint8_t>({1, 2});
        return channelLayoutForLabels(kStereo);
      }

      return {};
    }

    void normalizeAlacSlots(std::span<std::byte> bytes,
                            std::uint8_t channels,
                            std::uint32_t sampleBytes,
                            AlacChannelLayout const& layout) noexcept
    {
      auto const frameBytes = static_cast<std::size_t>(channels) * sampleBytes;
      auto frame = std::array<std::byte, kMaxAlacChannels * sizeof(std::int32_t)>{};

      for (std::size_t offset = 0; offset < bytes.size(); offset += frameBytes)
      {
        std::memcpy(frame.data(), bytes.data() + offset, frameBytes);

        for (std::size_t destination = 0; destination < channels; ++destination)
        {
          std::memcpy(bytes.data() + offset + (destination * sampleBytes),
                      frame.data() + (static_cast<std::size_t>(layout.sourceSlots[destination]) * sampleBytes),
                      sampleBytes);
        }
      }
    }

    SampleEncoding nativeAlacEncoding(std::uint8_t const precisionBits) noexcept
    {
      if (precisionBits <= 16U)
      {
        return SampleEncoding::Signed16Le;
      }

      if (precisionBits <= 24U)
      {
        return SampleEncoding::Signed24PackedLe;
      }

      return SampleEncoding::Signed32Le;
    }
  } // namespace

  struct AlacDecoderSession::Impl final
  {
    detail::DecoderOutputAdapter outputAdapter;
    DecodedStreamInfo info;
    AlacChannelLayout channelLayout;
    bool requiresSlotNormalization = false;

    std::unique_ptr<ALACDecoder> decoderPtr;

    detail::Mp4PacketSource packetSource;

    std::vector<std::byte> targetPcm;

    Impl(std::optional<SampleEncoding> optOutputEncoding)
      : outputAdapter{optOutputEncoding}
    {
      decoderPtr = std::make_unique<ALACDecoder>();
    }

    std::uint32_t fallbackFrameLength() const noexcept
    {
      return decoderPtr->mConfig.frameLength > 0 ? decoderPtr->mConfig.frameLength
                                                 : static_cast<std::uint32_t>(kALACDefaultFramesPerPacket);
    }
  };

  AlacDecoderSession::AlacDecoderSession(std::optional<SampleEncoding> optOutputEncoding)
    : _implPtr{std::make_unique<Impl>(optOutputEncoding)}
  {
  }

  AlacDecoderSession::~AlacDecoderSession() = default;

  Result<> AlacDecoderSession::initialize(std::filesystem::path const& filePath) noexcept
  {
    try
    {
      if (auto const res = _implPtr->packetSource.open(filePath, "alac"); !res)
      {
        auto error = res.error();

        if (error.code == Error::Code::FormatRejected)
        {
          error.code = Error::Code::InitFailed;
        }

        detail::throwDecoderError(std::move(error));
      }

      auto const cookie = _implPtr->packetSource.magicCookie();

      validateAlacCookie(cookie);

      auto const initStatus =
        _implPtr->decoderPtr->Init(layout::asLegacyPtr<std::uint8_t>(cookie), layout::size32(cookie));

      if (initStatus != ALAC_noErr)
      {
        detail::throwDecoderError(Error::Code::InitFailed, "Failed to initialize ALAC decoder");
      }

      auto const& config = _implPtr->decoderPtr->mConfig;

      if (config.sampleRate == 0 || config.numChannels == 0 || config.bitDepth == 0)
      {
        detail::throwDecoderError(Error::Code::InitFailed, "Invalid ALAC stream configuration");
      }

      _implPtr->info.duration = _implPtr->packetSource.duration(config.sampleRate);
      _implPtr->info.codec = AudioCodec::Alac;

      _implPtr->info.sourceFormat.channels = config.numChannels;
      _implPtr->info.sourceFormat.sampleRate = config.sampleRate;
      _implPtr->info.sourceFormat.precisionBits = config.bitDepth;
      _implPtr->info.sourceFormat.sampleKind = SampleKind::Integer;
      _implPtr->channelLayout = channelLayoutForEntry(_implPtr->packetSource.alacSampleEntry(), config.numChannels);
      _implPtr->info.sourceFormat.speakerMask = _implPtr->channelLayout.speakerMask;
      _implPtr->info.sourceChannelLayout.origin =
        _implPtr->channelLayout.speakerMask != 0 ? ChannelLayoutOrigin::Identified : ChannelLayoutOrigin::Unknown;
      _implPtr->requiresSlotNormalization = false;

      if (_implPtr->channelLayout.speakerMask != 0)
      {
        for (std::uint8_t index = 0; index < config.numChannels; ++index)
        {
          _implPtr->requiresSlotNormalization =
            _implPtr->requiresSlotNormalization || _implPtr->channelLayout.sourceSlots[index] != index;
        }
      }

      auto const configuredRes = _implPtr->outputAdapter.configure(
        _implPtr->info.sourceFormat, nativeAlacEncoding(_implPtr->info.sourceFormat.precisionBits));

      if (!configuredRes)
      {
        detail::throwDecoderError(configuredRes.error());
      }

      _implPtr->info.outputFormat = *configuredRes;

      return {};
    }
    catch (detail::DecoderException const& ex)
    {
      return std::unexpected{ex.error()};
    }
  }

  Result<> AlacDecoderSession::seek(std::chrono::milliseconds offset) noexcept
  {
    return _implPtr->packetSource.seek(offset, _implPtr->info.sourceFormat.sampleRate);
  }

  void AlacDecoderSession::flush() noexcept
  {
  }

  // Result error materialization may allocate; DecoderSession intentionally
  // fails fast if an allocation escapes this noexcept boundary.
  Result<PcmBlock> AlacDecoderSession::readNextBlock() noexcept
  {
    if (_implPtr->packetSource.isAtEnd())
    {
      return PcmBlock{.endOfStream = true};
    }

    auto const firstFrameIndex =
      _implPtr->packetSource.firstFrameIndex(_implPtr->info.sourceFormat.sampleRate, _implPtr->fallbackFrameLength());
    auto const packet = _implPtr->packetSource.packet();

    if (packet.empty())
    {
      return makeError(Error::Code::DecodeFailed, "Failed to read ALAC sample payload");
    }

    auto const maxFrames = (_implPtr->decoderPtr->mConfig.frameLength > 0)
                             ? _implPtr->decoderPtr->mConfig.frameLength
                             : static_cast<std::uint32_t>(kALACDefaultFramesPerPacket);

    auto const sourceBps = _implPtr->info.sourceFormat.precisionBits;
    auto const channels = _implPtr->info.outputFormat.channels;

    auto const nativeSampleBytes = static_cast<std::uint32_t>((sourceBps + 7U) / 8U);
    auto const nativeBytesPerFrame = channels * nativeSampleBytes;

    if (nativeBytesPerFrame == 0)
    {
      return makeError(Error::Code::DecodeFailed, "Invalid ALAC format calculation");
    }

    std::uint32_t frameCount = 0;

    _implPtr->targetPcm.resize(static_cast<std::size_t>(maxFrames) * nativeBytesPerFrame);

    auto bitBuffer = BitBuffer{};
    ::BitBufferInit(&bitBuffer, layout::asLegacyPtr<std::uint8_t>(packet), layout::size32(packet));

    auto const status = _implPtr->decoderPtr->Decode(
      &bitBuffer, layout::viewMutable<uint8_t>(std::span{_implPtr->targetPcm}), maxFrames, channels, &frameCount);

    if (status != 0)
    {
      return makeError(Error::Code::DecodeFailed, "ALAC decode failed");
    }

    if (frameCount == 0 || frameCount > maxFrames)
    {
      return makeError(Error::Code::DecodeFailed, "Invalid ALAC decoded frame count");
    }

    _implPtr->targetPcm.resize(static_cast<std::size_t>(frameCount) * nativeBytesPerFrame);

    if (_implPtr->requiresSlotNormalization)
    {
      normalizeAlacSlots(_implPtr->targetPcm, channels, nativeSampleBytes, _implPtr->channelLayout);
    }

    auto convertedRes = _implPtr->outputAdapter.convert(_implPtr->targetPcm);

    if (!convertedRes)
    {
      return std::unexpected{convertedRes.error()};
    }

    _implPtr->packetSource.advance();

    return PcmBlock{
      .bytes = *convertedRes,
      .frames = frameCount,
      .firstFrameIndex = firstFrameIndex,
      .endOfStream = _implPtr->packetSource.isAtEnd(),
    };
  }

  DecodedStreamInfo AlacDecoderSession::streamInfo() const noexcept
  {
    return _implPtr->info;
  }
} // namespace ao::audio
