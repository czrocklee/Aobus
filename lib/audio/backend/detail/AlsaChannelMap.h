// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/audio/Backend.h>
#include <ao/audio/ChannelDeliveryEvidence.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/SampleEncoding.h>

extern "C"
{
#include <alsa/asoundlib.h>
}

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace ao::audio::backend::detail
{
  inline constexpr std::size_t kAlsaChannelCapacity =
    static_cast<std::size_t>(std::numeric_limits<decltype(PcmFormat::channels)>::max());
  // S24-in-32, S32 and F32 are the widest sample containers this backend supports.
  inline constexpr std::size_t kAlsaMaxSampleBytes =
    static_cast<std::size_t>(bytesPerSample(SampleEncoding::Signed32Le));
  inline constexpr std::size_t kAlsaPermutationScratchBytes = kAlsaChannelCapacity * kAlsaMaxSampleBytes;

  // Parsed native results, not requested roles. A NULL capability or current
  // result is an independent availability fact: it never infers an unsupported
  // or failed category and never selects legacy admission.
  struct AlsaChannelMapObservations final
  {
    enum class Query : std::uint8_t
    {
      Unavailable,
      NoApplicableMap,
      // A matching-count offer, including partial or legacy-ambiguous roles;
      // neither complete position evidence nor proof of current mapping.
      ApplicableMap,
    };

    Query query = Query::Unavailable;
    std::vector<std::vector<unsigned int>> supportedMaps{};
    std::optional<std::vector<unsigned int>> optCurrentPositions{};
  };

  struct AlsaChannelMap final
  {
    AlsaChannelMapObservations observations;
    ChannelDeliveryEvidence delivery{.isMapped = false};
    std::uint8_t channels = 0;
    std::uint8_t sampleBytes = 0;
    bool needsPermutation = false;
    // Destination native slot -> canonical application slot. Fixed storage is
    // bounded by PcmFormat's uint8_t count; only `channels` entries are used.
    std::array<std::uint8_t, kAlsaChannelCapacity> sourceSlots{};
  };

  using AlsaChannelMapReader = std::function<std::expected<AlsaChannelMapObservations, BackendOpenFailure>()>;

  std::expected<AlsaChannelMapObservations, BackendOpenFailure> readAlsaChannelMaps(::snd_pcm_t* pcm);

  // Called only after applied native storage and required precision succeed,
  // while open still owns the not-yet-started handle. The callback is the
  // parsed-value boundary shared by native open and deterministic fixtures.
  std::expected<AlsaChannelMap, BackendOpenFailure> prepareAlsaChannelMap(PcmFormat const& clientFormat,
                                                                          AlsaChannelMapReader const& readMaps);

  // Whole interleaved frames only. The caller provisions one frame of scratch
  // (at most kAlsaPermutationScratchBytes) before rendering. Copy-before-scatter makes cycles safe
  // in-place, without allocation, arithmetic conversion or padding rewrites.
  void applyAlsaChannelPermutation(std::span<std::byte> pcm,
                                   AlsaChannelMap const& map,
                                   std::span<std::byte> frameScratch) noexcept;
} // namespace ao::audio::backend::detail
