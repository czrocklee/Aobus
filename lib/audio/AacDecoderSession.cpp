// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "AacDecoderSession.h"

#include "detail/DecoderError.h"
#include "detail/DecoderOutput.h"
#include "detail/DecoderOutputAdapter.h"
#include "detail/Mp4PacketSource.h"
#include <ao/AudioCodec.h>
#include <ao/Error.h>
#include <ao/audio/ChannelLayoutEvidence.h>
#include <ao/audio/DecodedStreamInfo.h>
#include <ao/audio/PcmBlock.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>
#include <ao/utility/Raii.h>

#include <fdk-aac/FDK_audio.h>
#include <fdk-aac/aacdecoder_lib.h>
#include <fdk-aac/machine_type.h>

#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace ao::audio
{
  namespace
  {
    constexpr std::uint8_t kAacPcmBitDepth = 16;
    constexpr std::uint32_t kAacMaxFrameSize = 2048;
    constexpr std::uint8_t kAacMaxChannels = 8;
    static_assert(sizeof(::INT_PCM) == sizeof(std::int16_t), "AAC native PCM must use 16-bit samples");

    using AacDecoderPtr = decltype(utility::makeUniquePtr<::aacDecoder_Close>(::HANDLE_AACDECODER{}));

    struct AacChannelSignature final
    {
      bool hasTypes = false;
      bool hasIndices = false;
      std::array<::AUDIO_CHANNEL_TYPE, kAacMaxChannels> types{};
      std::array<::UCHAR, kAacMaxChannels> indices{};

      bool operator==(AacChannelSignature const&) const = default;
    };

    struct AacFrameSnapshot final
    {
      std::uint32_t sampleRate = 0;
      std::uint32_t frames = 0;
      std::uint8_t channels = 0;
      AacChannelSignature channelSignature;
    };

    struct AacChannelPlan final
    {
      std::uint32_t speakerMask = 0;
      bool needsPermutation = false;
      // Each canonical destination slot names its native source slot.
      std::array<std::uint8_t, kAacMaxChannels> sourceSlots{};
    };

    AacDecoderPtr openAacDecoder()
    {
      auto decoderPtr = AacDecoderPtr{::aacDecoder_Open(TT_MP4_RAW, 1)};

      if (!decoderPtr)
      {
        detail::throwDecoderError(Error::Code::InitFailed, "Failed to create AAC decoder");
      }

      if (::aacDecoder_SetParam(decoderPtr.get(), AAC_PCM_OUTPUT_CHANNEL_MAPPING, 1) != AAC_DEC_OK)
      {
        detail::throwDecoderError(Error::Code::InitFailed, "Failed to configure AAC channel mapping");
      }

      // FDK defaults can downmix eight coded channels to six output channels.
      // Preserve the encoded channel count instead of adapting that mixed signal.
      if (::aacDecoder_SetParam(decoderPtr.get(), AAC_PCM_MAX_OUTPUT_CHANNELS, 0) != AAC_DEC_OK)
      {
        detail::throwDecoderError(Error::Code::InitFailed, "Failed to disable AAC channel downmixing");
      }

      if (::aacDecoder_SetParam(decoderPtr.get(), AAC_PCM_MIN_OUTPUT_CHANNELS, 0) != AAC_DEC_OK)
      {
        detail::throwDecoderError(Error::Code::InitFailed, "Failed to disable AAC channel extension");
      }

      return decoderPtr;
    }

    AacFrameSnapshot aacFrameSnapshot(::HANDLE_AACDECODER decoder)
    {
      auto const* const streamInfo = ::aacDecoder_GetStreamInfo(decoder);

      if (streamInfo == nullptr || streamInfo->sampleRate <= 0 || streamInfo->frameSize <= 0 ||
          streamInfo->numChannels <= 0 || std::cmp_greater(streamInfo->frameSize, kAacMaxFrameSize) ||
          std::cmp_greater(streamInfo->numChannels, kAacMaxChannels))
      {
        detail::throwDecoderError(Error::Code::DecodeFailed, "Invalid AAC stream information");
      }

      auto snapshot = AacFrameSnapshot{
        .sampleRate = static_cast<std::uint32_t>(streamInfo->sampleRate),
        .frames = static_cast<std::uint32_t>(streamInfo->frameSize),
        .channels = static_cast<std::uint8_t>(streamInfo->numChannels),
        .channelSignature =
          {
            .hasTypes = streamInfo->pChannelType != nullptr,
            .hasIndices = streamInfo->pChannelIndices != nullptr,
          },
      };

      // FDK owns these arrays and may replace their contents on the next decode.
      for (std::size_t slot = 0; slot < snapshot.channels; ++slot)
      {
        if (snapshot.channelSignature.hasTypes)
        {
          snapshot.channelSignature.types[slot] = streamInfo->pChannelType[slot];
        }

        if (snapshot.channelSignature.hasIndices)
        {
          snapshot.channelSignature.indices[slot] = streamInfo->pChannelIndices[slot];
        }
      }

      return snapshot;
    }

    std::uint32_t resolveAacWaveRole(::AUDIO_CHANNEL_TYPE type, ::UCHAR index, std::uint8_t groupPopulation) noexcept
    {
      if (index >= groupPopulation)
      {
        return 0;
      }

      constexpr auto kFrontPairRoles = std::to_array<std::uint32_t>({0x1, 0x2});
      constexpr auto kFrontCenterFirstRoles = std::to_array<std::uint32_t>({0x4, 0x1, 0x2});
      constexpr auto kBackPairRoles = std::to_array<std::uint32_t>({0x10, 0x20});
      constexpr auto kSidePairRoles = std::to_array<std::uint32_t>({0x200, 0x400});
      constexpr std::uint32_t kBackCenterRole = 0x100;
      constexpr std::uint32_t kLfeRole = 0x8;

      switch (type)
      {
        case ::ACT_FRONT:
          if (groupPopulation == 2)
          {
            return kFrontPairRoles[index];
          }

          if (groupPopulation == 1 || groupPopulation == 3)
          {
            return kFrontCenterFirstRoles[index];
          }

          return 0;

        case ::ACT_BACK:
          if (groupPopulation == 1)
          {
            return kBackCenterRole;
          }

          if (groupPopulation == 2)
          {
            return kBackPairRoles[index];
          }

          return 0;

        case ::ACT_SIDE:
          if (groupPopulation == 2)
          {
            return kSidePairRoles[index];
          }

          return 0;

        case ::ACT_LFE:
          if (groupPopulation == 1)
          {
            return kLfeRole;
          }

          return 0;

        default: return 0;
      }
    }

    AacChannelPlan planAacChannels(AacFrameSnapshot const& snapshot) noexcept
    {
      auto const& signature = snapshot.channelSignature;

      if (!signature.hasTypes || !signature.hasIndices)
      {
        return {};
      }

      std::uint8_t frontCount = 0;
      std::uint8_t backCount = 0;
      std::uint8_t sideCount = 0;
      std::uint8_t lfeCount = 0;

      for (std::size_t slot = 0; slot < snapshot.channels; ++slot)
      {
        switch (signature.types[slot])
        {
          case ::ACT_FRONT: ++frontCount; break;
          case ::ACT_BACK: ++backCount; break;
          case ::ACT_SIDE: ++sideCount; break;
          case ::ACT_LFE: ++lfeCount; break;
          default: return {};
        }
      }

      auto roles = std::array<std::uint32_t, kAacMaxChannels>{};
      std::uint32_t mask = 0;

      for (std::size_t slot = 0; slot < snapshot.channels; ++slot)
      {
        auto const type = signature.types[slot];
        std::uint8_t groupPopulation = 0;

        switch (type)
        {
          case ::ACT_FRONT: groupPopulation = frontCount; break;
          case ::ACT_BACK: groupPopulation = backCount; break;
          case ::ACT_SIDE: groupPopulation = sideCount; break;
          case ::ACT_LFE: groupPopulation = lfeCount; break;
          default: return {};
        }

        auto const role = resolveAacWaveRole(type, signature.indices[slot], groupPopulation);

        // An exact population with distinct in-range indices has no holes.
        if (role == 0 || (mask & role) != 0)
        {
          return {};
        }

        roles[slot] = role;
        mask |= role;
      }

      if (!isCompleteWaveSpeakerMask(mask, snapshot.channels))
      {
        return {};
      }

      auto plan = AacChannelPlan{.speakerMask = mask};

      for (std::size_t sourceSlot = 0; sourceSlot < snapshot.channels; ++sourceSlot)
      {
        auto const destinationSlot = static_cast<std::size_t>(std::popcount(mask & (roles[sourceSlot] - 1U)));

        plan.sourceSlots[destinationSlot] = static_cast<std::uint8_t>(sourceSlot);
        plan.needsPermutation = plan.needsPermutation || destinationSlot != sourceSlot;
      }

      return plan;
    }
  } // namespace

  struct AacDecoderSession::Impl final
  {
    std::optional<SampleEncoding> const optRequestedEncoding;
    detail::DecoderOutputAdapter outputAdapter;
    DecodedStreamInfo info;
    AacDecoderPtr decoderPtr;
    detail::Mp4PacketSource packetSource;
    std::vector<::UCHAR> inputBuffer;
    std::vector<::INT_PCM> pcmBuffer;
    AacChannelSignature channelSignature;
    AacChannelPlan channelPlan;
    std::optional<PcmBlock> optPendingFirstBlock;

    explicit Impl(std::optional<SampleEncoding> optOutputEncoding)
      : optRequestedEncoding{optOutputEncoding}, outputAdapter{optOutputEncoding}
    {
    }

    ~Impl() = default;

    Impl(Impl const&) = delete;
    Impl& operator=(Impl const&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    void configureDecoder(::HANDLE_AACDECODER nativeDecoder, std::span<std::byte const> magicCookie)
    {
      if (magicCookie.empty())
      {
        detail::throwDecoderError(Error::Code::FormatRejected, "Missing AAC AudioSpecificConfig");
      }

      if (!std::in_range<::UINT>(magicCookie.size()))
      {
        detail::throwDecoderError(Error::Code::InitFailed, "AAC configuration exceeds decoder input bounds");
      }

      // UCHAR and std::byte are both byte-sized, so this is a straight copy.
      inputBuffer.resize(magicCookie.size());
      std::memcpy(inputBuffer.data(), magicCookie.data(), magicCookie.size());

      auto const configRes = [&]
      {
        auto configData = std::array{inputBuffer.data()};
        auto configSize = std::to_array<::UINT>({static_cast<::UINT>(inputBuffer.size())});
        return ::aacDecoder_ConfigRaw(nativeDecoder, configData.data(), configSize.data());
      }();

      if (configRes != AAC_DEC_OK)
      {
        detail::throwDecoderError(Error::Code::InitFailed, "Failed to configure AAC decoder");
      }

      if (auto const* streamInfo = ::aacDecoder_GetStreamInfo(nativeDecoder);
          streamInfo != nullptr && (std::cmp_greater(streamInfo->numChannels, kAacMaxChannels) ||
                                    std::cmp_greater(streamInfo->frameSize, kAacMaxFrameSize)))
      {
        detail::throwDecoderError(Error::Code::InitFailed, "Unsupported AAC stream dimensions");
      }
    }

    void validateRequestedEncoding() const
    {
      auto const sourceFacts = SignalFormat{
        .precisionBits = kAacPcmBitDepth,
        .sampleKind = SampleKind::Integer,
      };

      if (optRequestedEncoding && !detail::isLosslessPcmEncoding(sourceFacts, *optRequestedEncoding))
      {
        detail::throwDecoderError(Error::Code::NotSupported, "Requested decoder PCM encoding loses source precision");
      }
    }

    void finalizeStreamInfo(AacFrameSnapshot const& snapshot)
    {
      channelSignature = snapshot.channelSignature;
      channelPlan = planAacChannels(snapshot);
      info.sourceFormat = SignalFormat{
        .sampleRate = snapshot.sampleRate,
        .channels = snapshot.channels,
        .precisionBits = kAacPcmBitDepth,
        .sampleKind = SampleKind::Integer,
        .speakerMask = channelPlan.speakerMask,
      };
      info.sourceChannelLayout.origin =
        channelPlan.speakerMask != 0 ? ChannelLayoutOrigin::Identified : ChannelLayoutOrigin::Unknown;
      info.isLossy = true;
      info.codec = AudioCodec::Aac;

      auto const configuredRes = outputAdapter.configure(info.sourceFormat, SampleEncoding::Signed16Le);

      if (!configuredRes)
      {
        detail::throwDecoderError(configuredRes.error());
      }

      info.outputFormat = *configuredRes;
    }

    void normalizePcm(std::span<::INT_PCM> activePcm) noexcept
    {
      if (!channelPlan.needsPermutation)
      {
        return;
      }

      auto samples = std::array<::INT_PCM, kAacMaxChannels>{};
      auto const channels = info.sourceFormat.channels;

      for (std::size_t frameOffset = 0; frameOffset < activePcm.size(); frameOffset += channels)
      {
        for (std::size_t slot = 0; slot < channels; ++slot)
        {
          samples[slot] = activePcm[frameOffset + channelPlan.sourceSlots[slot]];
        }

        for (std::size_t slot = 0; slot < channels; ++slot)
        {
          activePcm[frameOffset + slot] = samples[slot];
        }
      }
    }

    PcmBlock decodePacket(bool isPriming)
    {
      auto const packet = packetSource.packet();

      if (packet.empty())
      {
        detail::throwDecoderError(Error::Code::DecodeFailed, "AAC sample payload is empty or inaccessible");
      }

      if (!std::in_range<::UINT>(packet.size()))
      {
        detail::throwDecoderError(Error::Code::DecodeFailed, "AAC sample exceeds decoder input bounds");
      }

      inputBuffer.resize(packet.size());
      std::memcpy(inputBuffer.data(), packet.data(), packet.size());

      auto bytesValid = static_cast<::UINT>(inputBuffer.size());
      auto const fillRes = [&]
      {
        auto inputData = std::array{inputBuffer.data()};
        auto inputSize = std::to_array<::UINT>({static_cast<::UINT>(inputBuffer.size())});
        return ::aacDecoder_Fill(decoderPtr.get(), inputData.data(), inputSize.data(), &bytesValid);
      }();

      if (fillRes != AAC_DEC_OK)
      {
        detail::throwDecoderError(Error::Code::DecodeFailed, "Failed to fill AAC decoder input");
      }

      if (bytesValid != 0)
      {
        detail::throwDecoderError(Error::Code::DecodeFailed, "AAC decoder did not consume the complete sample");
      }

      if (auto const* streamInfo = ::aacDecoder_GetStreamInfo(decoderPtr.get());
          streamInfo != nullptr && (std::cmp_greater(streamInfo->numChannels, kAacMaxChannels) ||
                                    std::cmp_greater(streamInfo->frameSize, kAacMaxFrameSize)))
      {
        detail::throwDecoderError(Error::Code::DecodeFailed, "Unsupported AAC stream dimensions");
      }

      // ConfigRaw may leave geometry zero; later frames may change frame size.
      pcmBuffer.resize(static_cast<std::size_t>(kAacMaxFrameSize) * kAacMaxChannels);

      auto const decodeRes =
        ::aacDecoder_DecodeFrame(decoderPtr.get(), pcmBuffer.data(), static_cast<::INT>(pcmBuffer.size()), 0);

      if (decodeRes != AAC_DEC_OK)
      {
        detail::throwDecoderError(Error::Code::DecodeFailed, "AAC decode failed");
      }

      auto const snapshot = aacFrameSnapshot(decoderPtr.get());

      if (isPriming)
      {
        finalizeStreamInfo(snapshot);
      }
      else if (snapshot.sampleRate != info.sourceFormat.sampleRate || snapshot.channels != info.sourceFormat.channels ||
               snapshot.channelSignature != channelSignature)
      {
        detail::throwDecoderError(
          Error::Code::NotSupported, "AAC stream changed output format or channel identity during playback");
      }

      auto const samples = static_cast<std::size_t>(snapshot.frames) * snapshot.channels;
      auto const firstFrameIndex = packetSource.firstFrameIndex(snapshot.sampleRate, snapshot.frames);

      auto const activePcm = std::span<::INT_PCM>{pcmBuffer}.first(samples);
      normalizePcm(activePcm);
      packetSource.advance();

      auto const nativeBytes = std::as_bytes(activePcm);
      auto convertedRes = outputAdapter.convert(nativeBytes);

      if (!convertedRes)
      {
        detail::throwDecoderError(convertedRes.error());
      }

      return PcmBlock{
        .bytes = *convertedRes,
        .frames = snapshot.frames,
        .firstFrameIndex = firstFrameIndex,
        .endOfStream = packetSource.isAtEnd(),
      };
    }
  };

  AacDecoderSession::AacDecoderSession(std::optional<SampleEncoding> optOutputEncoding)
    : _implPtr{std::make_unique<Impl>(optOutputEncoding)}
  {
  }

  AacDecoderSession::~AacDecoderSession() = default;

  Result<> AacDecoderSession::initialize(std::filesystem::path const& filePath) noexcept
  {
    try
    {
      _implPtr->decoderPtr = openAacDecoder();

      if (auto const res = _implPtr->packetSource.open(filePath, "mp4a"); !res)
      {
        auto error = res.error();

        if (error.code == Error::Code::FormatRejected)
        {
          error.code = Error::Code::InitFailed;
        }

        detail::throwDecoderError(std::move(error));
      }

      _implPtr->configureDecoder(_implPtr->decoderPtr.get(), _implPtr->packetSource.magicCookie());
      _implPtr->validateRequestedEncoding();

      _implPtr->info.duration = _implPtr->packetSource.duration();
      _implPtr->optPendingFirstBlock = _implPtr->decodePacket(true);

      return {};
    }
    catch (detail::DecoderException const& ex)
    {
      return std::unexpected{ex.error()};
    }
  }

  // Result materialization and initial-seek configuration may allocate;
  // DecoderSession intentionally fails fast if an allocation escapes this boundary.
  Result<> AacDecoderSession::seek(std::chrono::milliseconds offset) noexcept
  {
    try
    {
      if (_implPtr->optPendingFirstBlock)
      {
        // Preserve legacy cold-start history if the first public read is a seek.
        // Prepare before moving the source cursor so any failure retains packet 0.
        auto candidatePtr = openAacDecoder();
        _implPtr->configureDecoder(candidatePtr.get(), _implPtr->packetSource.magicCookie());

        if (auto const res = _implPtr->packetSource.seek(offset); !res)
        {
          return std::unexpected{res.error()};
        }

        if (_implPtr->packetSource.sampleIndex() == 0)
        {
          // Keep the original primed PCM and restore its next-packet cursor.
          _implPtr->packetSource.advance();
        }
        else
        {
          _implPtr->decoderPtr.swap(candidatePtr);
          _implPtr->optPendingFirstBlock.reset();
        }

        return {};
      }

      if (auto const res = _implPtr->packetSource.seek(offset); !res)
      {
        return std::unexpected{res.error()};
      }

      flush();
      return {};
    }
    catch (detail::DecoderException const& ex)
    {
      auto error = ex.error();
      error.code = Error::Code::SeekFailed;
      return std::unexpected{std::move(error)};
    }
  }

  void AacDecoderSession::flush() noexcept
  {
    if (_implPtr->decoderPtr)
    {
      std::ignore = ::aacDecoder_SetParam(_implPtr->decoderPtr.get(), AAC_TPDEC_CLEAR_BUFFER, 1);
    }
  }

  // Result error materialization and decode buffers may allocate; DecoderSession
  // intentionally fails fast if an allocation escapes this noexcept boundary.
  Result<PcmBlock> AacDecoderSession::readNextBlock() noexcept
  {
    try
    {
      if (_implPtr->optPendingFirstBlock)
      {
        auto const block = *_implPtr->optPendingFirstBlock;
        _implPtr->optPendingFirstBlock.reset();
        return block;
      }

      if (_implPtr->packetSource.isAtEnd())
      {
        return PcmBlock{.endOfStream = true};
      }

      return _implPtr->decodePacket(false);
    }
    catch (detail::DecoderException const& ex)
    {
      return std::unexpected{ex.error()};
    }
  }

  DecodedStreamInfo AacDecoderSession::streamInfo() const noexcept
  {
    return _implPtr->info;
  }
} // namespace ao::audio
