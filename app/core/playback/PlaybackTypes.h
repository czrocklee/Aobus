// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "../AudioFormat.h"
#include <rs/core/Type.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace app::core::playback
{

  // Common audio sample type for internal PCM representation
  using AudioSample = float;

  enum class TransportState
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

  enum class BackendKind
  {
    None,
    PipeWire,          // shared/mixing mode
    PipeWireExclusive, // exclusive/direct hardware
    AlsaExclusive,
  };

  /**
   * @brief Helper to get the user-friendly name for a backend kind.
   */
  inline std::string_view backendDisplayName(BackendKind kind)
  {
    switch (kind)
    {
      case BackendKind::PipeWire: return "PipeWire (Shared)";
      case BackendKind::PipeWireExclusive: return "PipeWire (Exclusive)";
      case BackendKind::AlsaExclusive: return "ALSA (Exclusive)";
      default: return "Unknown";
    }
  }

  /**
   * @brief Helper to get the shorthand name for a backend kind.
   */
  inline std::string_view backendShortName(BackendKind kind)
  {
    switch (kind)
    {
      case BackendKind::PipeWire: return "PipeWire(S)";
      case BackendKind::PipeWireExclusive: return "PipeWire(E)";
      case BackendKind::AlsaExclusive: return "ALSA(E)";
      default: return "Output";
    }
  }

  /**
   * @brief Helper to get the internal ID string for a backend kind (used for actions/serialization).
   */
  inline std::string_view backendKindToId(BackendKind kind)
  {
    switch (kind)
    {
      case BackendKind::PipeWire: return "pipewire";
      case BackendKind::PipeWireExclusive: return "pipewire_exclusive";
      case BackendKind::AlsaExclusive: return "alsa";
      default: return "none";
    }
  }

  /**
   * @brief Helper to get the backend kind from an internal ID string.
   */
  inline BackendKind backendKindFromId(std::string_view id)
  {
    if (id == "pipewire") return BackendKind::PipeWire;

    if (id == "pipewire_exclusive") return BackendKind::PipeWireExclusive;

    if (id == "alsa") return BackendKind::AlsaExclusive;

    return BackendKind::None;
  }

  struct TrackPlaybackDescriptor final
  {
    rs::core::TrackId trackId{};
    std::filesystem::path filePath;
    std::string title;
    std::string artist;
    std::string album;
    std::optional<rs::core::ResourceId> coverArtId;
    std::uint32_t durationMs = 0;
    std::uint32_t sampleRateHint = 0;
    std::uint8_t channelsHint = 0;
    std::uint8_t bitDepthHint = 0;
  };

  /**
   * @brief Type of an audio processing node in the playback graph.
   */
  enum class AudioNodeType
  {
    Decoder,        ///< Source decoder (e.g. FFmpeg)
    Engine,         ///< RockStudio internal engine (volume, EQ, etc)
    Stream,         ///< Backend-specific stream node (e.g. PipeWire stream)
    Intermediary,   ///< Generic processing node (e.g. PipeWire filter)
    Sink,           ///< Final hardware output device
    ExternalSource, ///< Another application feeding into a node (mixing)
  };

  /**
   * @brief Represents a logical output device within a backend.
   */
  struct AudioDevice final
  {
    std::string id;          ///< Unique ID (backend specific, e.g. "hw:0,0" or node ID)
    std::string displayName; ///< User-friendly name
    std::string description; ///< Secondary details (e.g. backend/plugin name)
    bool isDefault = false;  ///< True if this is the system/backend default
    BackendKind backendKind = BackendKind::None;

    bool operator==(AudioDevice const&) const = default;
  };

  /**
   * @brief Represents a semantic component in the audio pipeline.
   */
  struct AudioNode final
  {
    std::string id = "";
    AudioNodeType type = AudioNodeType::Intermediary;
    std::string name = "";
    std::optional<AudioFormat> format = std::nullopt;
    bool volumeNotUnity = false;
    bool isMuted = false;
    bool isLossySource = false;
    std::string objectPath = "";

    bool operator==(AudioNode const&) const = default;
  };

  /**
   * @brief Represents a connection between two audio nodes.
   */
  struct AudioLink final
  {
    std::string sourceId = "";
    std::string destId = "";
    bool isActive = true;

    bool operator==(AudioLink const&) const = default;
  };

  /**
   * @brief Topological representation of the entire playback path.
   */
  struct AudioGraph final
  {
    std::vector<AudioNode> nodes;
    std::vector<AudioLink> links;

    bool operator==(AudioGraph const&) const = default;
  };

  /**
   * @brief Final conclusion on the quality of the current audio path.
   * Priority (higher value = more degraded/important to show):
   * BitwisePerfect < LosslessPadded == LosslessFloat < LinearIntervention < LossySource < Clipped
   */
  enum class AudioQuality
  {
    Unknown,
    BitwisePerfect,     ///< Purple: Bit-perfect, exclusive, no SRC/Vol/DSP
    LosslessPadded,     ///< Green: Bit-depth upscaling (integer), no SRC/Vol/DSP
    LosslessFloat,      ///< Green: Linear normalization to float, no SRC/Vol/DSP
    LinearIntervention, ///< Amber: Mixed, Resampled, Vol-Adjusted, or DSP
    LossySource,        ///< Gray: Source is lossy (MP3, AAC, etc.)
    Clipped,            ///< Red: Signal clipping detected
  };

  /**
   * @brief Represents the current state of a backend and its devices.
   */
  struct BackendSnapshot final
  {
    BackendKind kind = BackendKind::None;
    std::string displayName;
    std::string shortName;
    std::string id;
    std::vector<AudioDevice> devices;

    bool operator==(BackendSnapshot const&) const = default;
  };

  struct PlaybackSnapshot final
  {
    TransportState state = TransportState::Idle;
    BackendKind backend = BackendKind::None;
    std::string trackTitle;
    std::string trackArtist;
    std::uint32_t positionMs = 0;
    std::uint32_t durationMs = 0;
    std::uint32_t bufferedMs = 0;
    std::uint32_t underrunCount = 0;
    std::string statusText;

    // Device info
    std::string currentDeviceId;
    std::vector<BackendSnapshot> availableBackends;

    // Semantic graph data
    AudioGraph graph;
    AudioQuality quality = AudioQuality::Unknown;
    std::string qualityTooltip;
  };

} // namespace app::core::playback
