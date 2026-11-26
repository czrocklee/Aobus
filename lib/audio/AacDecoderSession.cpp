// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "AacDecoderSession.h"

#include "detail/DecoderError.h"
#include "detail/DecoderOutput.h"
#include "detail/DecoderOutputAdapter.h"
#include "detail/Mp4PacketSource.h"
#include "detail/TimeConversion.h"
#include <ao/AudioCodec.h>
#include <ao/Error.h>
#include <ao/audio/ChannelLayoutEvidence.h>
#include <ao/audio/DecodedStreamInfo.h>
#include <ao/audio/PcmBlock.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>
#include <ao/utility/Raii.h>

#include <fdk-aac/FDK_audio.h>
#include <fdk-aac/aacdecoder_lib.h>
#include <fdk-aac/machine_type.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <utility>
#include <vector>

namespace ao::audio
{
  namespace
  {
    constexpr std::uint8_t kAacPcmBitDepth = 16;
    constexpr std::uint32_t kAacMaxFrameSize = 2048;
    constexpr auto kAacLcFrameLengths = std::to_array<std::int32_t>({960, 1024});
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
      std::uint32_t outputDelay = 0;
      bool canRestartLc = false;
      std::uint8_t channels = 0;
      AacChannelSignature channelSignature;
    };

    struct AacRestartPosition final
    {
      std::uint32_t packetIndex = 0;
      std::uint64_t frameIndex = 0;
    };

    struct AacStoredPacket final
    {
      std::uint64_t firstFrameIndex = 0;
      std::vector<::INT_PCM> samples;
    };

    using AacRecentPcm = std::deque<AacStoredPacket>;

    struct AacRestart final
    {
      AacDecoderPtr decoderPtr;
      AacRestartPosition position;
      AacRecentPcm recent;
    };

    bool containsFrame(AacRecentPcm const& recent, std::uint64_t end, std::uint64_t target) noexcept
    {
      return !recent.empty() && recent.front().firstFrameIndex <= target && target < end;
    }

    // Ten seconds of rewind plus streaming read-ahead. Keep native s16, not
    // expanded output. Bound storage even for unusual rates or tiny AUs.
    constexpr std::uint32_t kRewindSeconds = 12;
    constexpr std::size_t kRewindBytes = std::size_t{20} * 1024 * 1024;
    constexpr std::size_t kRewindPackets = 2048;

    void rememberPacket(AacRecentPcm& recent,
                        std::uint32_t frameLimit,
                        std::uint64_t firstFrameIndex,
                        std::uint32_t frames,
                        std::span<::INT_PCM const> samples)
    {
      auto const end = firstFrameIndex + frames;
      auto storage = std::vector<::INT_PCM>{};

      while (!recent.empty() && (end - recent.front().firstFrameIndex > frameLimit || recent.size() >= kRewindPackets))
      {
        // Reuse a same-sized AU without retaining oversized allocations when
        // frame lengths change. A full steady-state window only copies PCM.
        if (storage.empty() && recent.front().samples.size() == samples.size())
        {
          storage = std::move(recent.front().samples);
        }

        recent.pop_front();
      }

      if (frames <= frameLimit)
      {
        storage.assign(samples.begin(), samples.end());
        recent.push_back({.firstFrameIndex = firstFrameIndex, .samples = std::move(storage)});
      }
    }

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
        .outputDelay = streamInfo->outputDelay,
        // Single-rate HE-AAC also has an LC core and unchanged geometry, but
        // needs its extension history. An ASC can name SBR solely to signal
        // its absence. Keep the shortcut exclusive to unextended LC; unknown
        // flags must not implicitly opt a codec tool into the LC restart rule.
        .canRestartLc = streamInfo->aot == ::AOT_AAC_LC && streamInfo->extSamplingRate == 0 && streamInfo->flags == 0 &&
                        streamInfo->aacSampleRate == streamInfo->sampleRate &&
                        std::ranges::contains(kAacLcFrameLengths, streamInfo->aacSamplesPerFrame) &&
                        streamInfo->aacSamplesPerFrame == streamInfo->frameSize,
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
    // One raw ASC configures the LC core rate and 960/1024-frame length
    // for the session. In-band metadata can disqualify future short restarts
    // without changing this stride or the cursor of the current decode.
    std::uint32_t configuredLcFrames = 0;
    std::uint32_t seekPrerollPackets = 0;
    std::uint64_t nextFrameIndex = 0;
    std::uint64_t discardBeforeFrame = 0;
    // At most one inactive, continuously decoded handle. It can seed a later
    // forward correction without mutating the active decoder during the scan.
    AacRestart checkpoint;
    // Contiguous validated PCM ending at the active decoder's next frame.
    // Replay moves only the public cursor; the FDK handle stays ready to
    // continue when replay catches up. Its saved checkpoint can likewise be
    // revisited on either side of a previous seek.
    AacRecentPcm recent;
    std::uint32_t rewindFrameLimit = 0;
    std::optional<std::size_t> optReplayPacket;
    bool atExplicitEnd = false;
    bool hasContinuousHistory = true;
    bool needsCleanRestart = false;

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
      if (snapshot.canRestartLc)
      {
        configuredLcFrames = snapshot.frames;
        // One transform overlap plus the packets still in FDK's delay line.
        auto const delayPackets = (snapshot.outputDelay + snapshot.frames - 1U) / snapshot.frames;
        seekPrerollPackets = 1U + delayPackets;
      }

      rewindFrameLimit = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(static_cast<std::uint64_t>(snapshot.sampleRate) * kRewindSeconds,
                                kRewindBytes / (sizeof(::INT_PCM) * snapshot.channels)));
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

    AacFrameSnapshot decodeNativePacket(::HANDLE_AACDECODER decoder,
                                        std::span<std::byte const> packet,
                                        std::vector<::INT_PCM>& pcm)
    {
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
        return ::aacDecoder_Fill(decoder, inputData.data(), inputSize.data(), &bytesValid);
      }();

      if (fillRes != AAC_DEC_OK)
      {
        detail::throwDecoderError(Error::Code::DecodeFailed, "Failed to fill AAC decoder input");
      }

      if (bytesValid != 0)
      {
        detail::throwDecoderError(Error::Code::DecodeFailed, "AAC decoder did not consume the complete sample");
      }

      if (auto const* streamInfo = ::aacDecoder_GetStreamInfo(decoder);
          streamInfo != nullptr && (std::cmp_greater(streamInfo->numChannels, kAacMaxChannels) ||
                                    std::cmp_greater(streamInfo->frameSize, kAacMaxFrameSize)))
      {
        detail::throwDecoderError(Error::Code::DecodeFailed, "Unsupported AAC stream dimensions");
      }

      // ConfigRaw may leave geometry zero; later frames may change frame size.
      pcm.resize(static_cast<std::size_t>(kAacMaxFrameSize) * kAacMaxChannels);

      auto const decodeRes = ::aacDecoder_DecodeFrame(decoder, pcm.data(), static_cast<::INT>(pcm.size()), 0);

      if (decodeRes != AAC_DEC_OK)
      {
        detail::throwDecoderError(Error::Code::DecodeFailed, "AAC decode failed");
      }

      return aacFrameSnapshot(decoder);
    }

    void validateIdentity(AacFrameSnapshot const& snapshot) const
    {
      if (snapshot.sampleRate != info.sourceFormat.sampleRate || snapshot.channels != info.sourceFormat.channels ||
          snapshot.channelSignature != channelSignature)
      {
        detail::throwDecoderError(
          Error::Code::NotSupported, "AAC stream changed output format or channel identity during playback");
      }
    }

    std::uint64_t targetFrame(std::chrono::milliseconds offset) const noexcept
    {
      return detail::saturatingScale(
        static_cast<std::uint64_t>(std::max<std::int64_t>(0, offset.count())), info.sourceFormat.sampleRate, 1000);
    }

    enum class SeekStart : std::uint8_t
    {
      End,
      Active,
      ShortLc,
      Checkpoint,
      Beginning,
    };

    static bool isWithinRead(std::uint64_t cursor, std::uint64_t target) noexcept
    {
      return cursor <= target && target - cursor < kAacMaxFrameSize;
    }

    static bool needsScan(std::uint64_t cursor, std::uint64_t target) noexcept
    {
      return cursor <= target && target - cursor >= kAacMaxFrameSize;
    }

    SeekStart seekStart(std::chrono::milliseconds offset, std::uint64_t target) const noexcept
    {
      if ((info.duration.count() > 0 && offset >= info.duration) ||
          target >= static_cast<std::uint64_t>(packetSource.sampleCount()) * kAacMaxFrameSize)
      {
        return SeekStart::End;
      }

      if (seekPrerollPackets != 0)
      {
        return SeekStart::ShortLc;
      }

      if (hasContinuousHistory &&
          (containsFrame(recent, nextFrameIndex, target) || isWithinRead(nextFrameIndex, target)))
      {
        return SeekStart::Active;
      }

      if (checkpoint.decoderPtr && (checkpoint.position.frameIndex <= target ||
                                    containsFrame(checkpoint.recent, checkpoint.position.frameIndex, target)))
      {
        return SeekStart::Checkpoint;
      }

      return SeekStart::Beginning;
    }

    void selectReadPosition(std::uint64_t target) noexcept
    {
      atExplicitEnd = false;
      optReplayPacket.reset();

      if (hasContinuousHistory && containsFrame(recent, nextFrameIndex, target))
      {
        auto const past = std::ranges::upper_bound(recent, target, {}, &AacStoredPacket::firstFrameIndex);
        optReplayPacket = static_cast<std::size_t>(past - recent.begin() - 1);
        optPendingFirstBlock.reset();
      }

      discardBeforeFrame = target;
    }

    void prepareRestart(AacRestart& candidate, SeekStart start, std::uint64_t target, std::stop_token const& stopToken)
    {
      auto& position = candidate.position;

      if (start == SeekStart::ShortLc)
      {
        auto const covering = target / configuredLcFrames;

        if (auto const count = packetSource.sampleCount(); covering >= count)
        {
          position = {.packetIndex = count};
          return;
        }

        auto const packet = static_cast<std::uint32_t>(covering);
        auto const index = packet - std::min(packet, seekPrerollPackets);
        position = {.packetIndex = index, .frameIndex = static_cast<std::uint64_t>(index) * configuredLcFrames};
        return;
      }

      scanForward(candidate.decoderPtr.get(), position, candidate.recent, target, stopToken);
    }

    void scanForward(::HANDLE_AACDECODER decoder,
                     AacRestartPosition& position,
                     AacRecentPcm& window,
                     std::uint64_t target,
                     std::stop_token const& stopToken)
    {
      auto const count = packetSource.sampleCount();
      // Stop before the next packet could contain the target. The first
      // public read has less than one maximum frame of discard left.
      auto scratch = std::vector<::INT_PCM>{};

      while (position.packetIndex < count && needsScan(position.frameIndex, target))
      {
        if (stopToken.stop_requested())
        {
          break;
        }

        auto const snapshot = decodeNativePacket(decoder, packetSource.packetAt(position.packetIndex), scratch);
        validateIdentity(snapshot);
        // Extend the window AU by AU: a prompt cancellation must
        // not discard its earlier PCM just because the request was far ahead.
        auto const samples = static_cast<std::size_t>(snapshot.frames) * snapshot.channels;
        rememberPacket(window,
                       rewindFrameLimit,
                       position.frameIndex,
                       snapshot.frames,
                       std::span<::INT_PCM const>{scratch}.first(samples));
        position.frameIndex += snapshot.frames;
        ++position.packetIndex;
      }
    }

    PcmBlock decodePacket(bool isPriming)
    {
      if (needsCleanRestart)
      {
        detail::throwDecoderError(Error::Code::DecodeFailed, "AAC frame contradicts the configured restart stride");
      }

      auto const snapshot = decodeNativePacket(decoderPtr.get(), packetSource.packet(), pcmBuffer);

      if (isPriming)
      {
        finalizeStreamInfo(snapshot);
      }
      else
      {
        validateIdentity(snapshot);

        if (!snapshot.canRestartLc || snapshot.frames != configuredLcFrames)
        {
          // For example, in-band DRC can appear on an ordinary LC access
          // unit. Its PCM still follows the previous unit at the same rate
          // and stride; only the eligibility of subsequent seeks changes.
          seekPrerollPackets = 0;
        }

        if (!hasContinuousHistory && configuredLcFrames != 0 && snapshot.frames != configuredLcFrames)
        {
          // A short restart inferred its origin from this ASC's core stride.
          // Do not expose speculative frame coordinates if that premise is
          // contradicted. Continuous decoding counts actual frames instead.
          needsCleanRestart = true;
          detail::throwDecoderError(Error::Code::DecodeFailed, "AAC frame contradicts the configured restart stride");
        }
      }

      auto const samples = static_cast<std::size_t>(snapshot.frames) * snapshot.channels;
      auto const firstFrameIndex = nextFrameIndex;
      nextFrameIndex += snapshot.frames;

      auto const activePcm = std::span<::INT_PCM>{pcmBuffer}.first(samples);

      if (seekPrerollPackets == 0 && hasContinuousHistory)
      {
        rememberPacket(recent, rewindFrameLimit, firstFrameIndex, snapshot.frames, activePcm);
      }

      packetSource.advance();
      return outputBlock(firstFrameIndex, activePcm, packetSource.isAtEnd());
    }

    PcmBlock nextPacket()
    {
      if (!optReplayPacket)
      {
        return decodePacket(false);
      }

      auto const& packet = recent[*optReplayPacket];
      // Returned spans must survive a later seek, even one replacing recent.
      pcmBuffer.assign(packet.samples.begin(), packet.samples.end());

      if (++*optReplayPacket == recent.size())
      {
        optReplayPacket.reset();
      }

      return outputBlock(packet.firstFrameIndex, pcmBuffer, !optReplayPacket && packetSource.isAtEnd());
    }

    PcmBlock outputBlock(std::uint64_t firstFrameIndex, std::span<::INT_PCM> activePcm, bool endOfStream)
    {
      normalizePcm(activePcm);
      auto const nativeBytes = std::as_bytes(activePcm);
      auto convertedRes = outputAdapter.convert(nativeBytes);

      if (!convertedRes)
      {
        detail::throwDecoderError(convertedRes.error());
      }

      return PcmBlock{
        .bytes = *convertedRes,
        .frames = static_cast<std::uint32_t>(activePcm.size() / info.sourceFormat.channels),
        .firstFrameIndex = firstFrameIndex,
        .endOfStream = endOfStream,
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

  bool AacDecoderSession::needsSeekWorker(std::chrono::milliseconds offset) const noexcept
  {
    switch (auto const target = _implPtr->targetFrame(offset); _implPtr->seekStart(offset, target))
    {
      case Impl::SeekStart::Checkpoint: return Impl::needsScan(_implPtr->checkpoint.position.frameIndex, target);
      case Impl::SeekStart::Beginning: return Impl::needsScan(0, target);
      default: return false;
    }
  }

  Result<> AacDecoderSession::seek(std::chrono::milliseconds offset) noexcept
  {
    return seekInterruptibly(offset, {});
  }

  Result<> AacDecoderSession::seekInterruptibly(std::chrono::milliseconds offset, std::stop_token stopToken) noexcept
  {
    if (stopToken.stop_requested())
    {
      return makeError(Error::Code::Conflict, "AAC seek cancelled");
    }

    auto& source = _implPtr->packetSource;
    auto const target = _implPtr->targetFrame(offset);
    auto const start = _implPtr->seekStart(offset, target);

    if (start == Impl::SeekStart::End)
    {
      // EOF needs no new decoder or history. Keep both validated windows for
      // a seek back; hide the old read cursor until a non-end seek succeeds.
      _implPtr->atExplicitEnd = true;
      return {};
    }

    if (start == Impl::SeekStart::Active)
    {
      _implPtr->optPendingFirstBlock.reset();
      _implPtr->selectReadPosition(target);
      return {};
    }

    auto candidate = AacRestart{};
    auto& checkpoint = _implPtr->checkpoint;

    if (start == Impl::SeekStart::Checkpoint)
    {
      candidate = std::exchange(checkpoint, AacRestart{});
    }
    else
    {
      try
      {
        candidate.decoderPtr = openAacDecoder();
        _implPtr->configureDecoder(candidate.decoderPtr.get(), source.magicCookie());
      }
      catch (detail::DecoderException const& ex)
      {
        auto error = ex.error();
        error.code = Error::Code::SeekFailed;
        return std::unexpected{std::move(error)};
      }
    }

    try
    {
      _implPtr->prepareRestart(candidate, start, target, stopToken);
      auto const& position = candidate.position;
      bool const needsHistory = start != Impl::SeekStart::ShortLc;

      if (stopToken.stop_requested())
      {
        // Restore a borrowed checkpoint or fill an empty slot. An unfinished
        // fresh scan cannot evict unrelated saved history. A completed one
        // wins even if stop arrived during the final AU, before installation.
        if (needsHistory && position.packetIndex != 0 &&
            (!checkpoint.decoderPtr || position.packetIndex == source.sampleCount() ||
             Impl::isWithinRead(position.frameIndex, target)))
        {
          checkpoint = std::move(candidate);
        }

        return makeError(Error::Code::Conflict, "AAC seek cancelled");
      }

      if (position.packetIndex != 0 || !_implPtr->optPendingFirstBlock)
      {
        if (needsHistory && _implPtr->hasContinuousHistory && (!source.isAtEnd() || !_implPtr->recent.empty()))
        {
          _implPtr->checkpoint = {
            .decoderPtr = std::move(_implPtr->decoderPtr),
            .position = {.packetIndex = source.sampleIndex(), .frameIndex = _implPtr->nextFrameIndex},
            .recent = std::move(_implPtr->recent)};
        }

        _implPtr->decoderPtr = std::move(candidate.decoderPtr);
        _implPtr->recent = std::move(candidate.recent);
        _implPtr->hasContinuousHistory = needsHistory || position.packetIndex == 0;
        _implPtr->needsCleanRestart = false;
        _implPtr->nextFrameIndex = position.frameIndex;
        _implPtr->optPendingFirstBlock.reset();
        source.setSampleIndex(position.packetIndex);
      }

      _implPtr->selectReadPosition(target);
      return {};
    }
    catch (detail::DecoderException const& ex)
    {
      return std::unexpected{ex.error()};
    }
  }

  Result<> AacDecoderSession::seekAfterDiscard(std::chrono::milliseconds offset, std::stop_token stopToken) noexcept
  {
    auto const target = _implPtr->targetFrame(offset);
    auto const start = _implPtr->seekStart(offset, target);
    // Prefer replay, a bounded restart, or a closer saved decoder. Otherwise
    // the discarded live cursor can advance exactly without re-decoding its prefix.
    bool const liveIsCloser =
      start == Impl::SeekStart::Beginning ||
      (start == Impl::SeekStart::Checkpoint && _implPtr->nextFrameIndex > _implPtr->checkpoint.position.frameIndex);

    if (!liveIsCloser || !_implPtr->hasContinuousHistory || !Impl::needsScan(_implPtr->nextFrameIndex, target))
    {
      return seekInterruptibly(offset, stopToken);
    }

    if (stopToken.stop_requested())
    {
      return makeError(Error::Code::Conflict, "AAC seek cancelled");
    }

    _implPtr->optPendingFirstBlock.reset();
    _implPtr->optReplayPacket.reset();
    auto& source = _implPtr->packetSource;
    auto position = AacRestartPosition{.packetIndex = source.sampleIndex(), .frameIndex = _implPtr->nextFrameIndex};

    try
    {
      _implPtr->scanForward(_implPtr->decoderPtr.get(), position, _implPtr->recent, target, stopToken);
    }
    catch (detail::DecoderException const& ex)
    {
      // Even a failed FDK call may have consumed input. A superseding seek
      // must use a validated checkpoint or a fresh handle, never this one.
      _implPtr->hasContinuousHistory = false;
      return std::unexpected{ex.error()};
    }

    source.setSampleIndex(position.packetIndex);
    _implPtr->nextFrameIndex = position.frameIndex;

    if (stopToken.stop_requested())
    {
      return makeError(Error::Code::Conflict, "AAC seek cancelled");
    }

    _implPtr->selectReadPosition(target);
    return {};
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
      while (!_implPtr->atExplicitEnd &&
             (_implPtr->optPendingFirstBlock || _implPtr->optReplayPacket || !_implPtr->packetSource.isAtEnd()))
      {
        auto const optPendingBlock = std::exchange(_implPtr->optPendingFirstBlock, std::nullopt);
        auto block = optPendingBlock ? *optPendingBlock : _implPtr->nextPacket();

        if (block.firstFrameIndex < _implPtr->discardBeforeFrame)
        {
          auto const skip = _implPtr->discardBeforeFrame - block.firstFrameIndex;

          if (skip >= block.frames)
          {
            continue;
          }

          block.bytes = block.bytes.subspan(static_cast<std::size_t>(skip) * frameBytes(_implPtr->info.outputFormat));
          block.frames -= static_cast<std::uint32_t>(skip);
          block.firstFrameIndex += skip;
        }

        _implPtr->discardBeforeFrame = 0;
        return block;
      }

      return PcmBlock{.endOfStream = true};
    }
    catch (detail::DecoderException const& ex)
    {
      _implPtr->hasContinuousHistory = false;
      return std::unexpected{ex.error()};
    }
  }

  DecodedStreamInfo AacDecoderSession::streamInfo() const noexcept
  {
    return _implPtr->info;
  }
} // namespace ao::audio
