// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/AudioCodec.h>
#include <ao/audio/Format.h>

#include <chrono>
#include <cstdint>
#include <filesystem>

namespace ao::audio
{
  // Common audio sample type for internal PCM representation
  using Sample = float;

  /**
   * @brief Snapshot of the active playback route's format chain.
   *
   * Lives in the public audio types header (rather than detail/) so that
   * Engine/Player status structs can embed it without pulling the RouteTracker
   * implementation into public headers.
   */
  struct RouteState final
  {
    Format sourceFormat;
    Format decoderOutputFormat;
    Format engineOutputFormat;
    bool isLossySource = false;
    AudioCodec codec = AudioCodec::Unknown;

    bool operator==(RouteState const&) const = default;
  };

  enum class Transport : std::uint8_t
  {
    Idle,
    Opening,
    Buffering,
    Playing,
    Paused,
    Seeking,
    Stopping,
    Error,
  };

  // Audio-only playback input. Library/runtime metadata stays above lib/audio.
  struct PlaybackInput final
  {
    std::filesystem::path filePath{};
    std::chrono::milliseconds duration{0};
    std::uint32_t sampleRateHint = 0;
    std::uint8_t channelsHint = 0;
    std::uint8_t bitDepthHint = 0;
  };

  /**
   * @brief Converts a duration to a sample count for a given sample rate.
   *
   * This centralizes the (duration.count() * sampleRate) / 1000 calculation,
   * providing a single point for potential precision or rounding adjustments.
   */
  constexpr std::uint64_t durationToSamples(std::chrono::milliseconds const duration,
                                            std::uint32_t const sampleRate) noexcept
  {
    return (static_cast<std::uint64_t>(duration.count()) * sampleRate) / 1000U;
  }

  /**
   * @brief Converts a sample count to a duration for a given sample rate.
   */
  constexpr std::chrono::milliseconds samplesToDuration(std::uint64_t const samples,
                                                        std::uint32_t const sampleRate) noexcept
  {
    if (sampleRate == 0)
    {
      return std::chrono::milliseconds{0};
    }

    return std::chrono::milliseconds{(samples * 1000U) / sampleRate};
  }
} // namespace ao::audio
