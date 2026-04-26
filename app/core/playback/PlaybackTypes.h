// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

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
    PipeWire,
    AlsaExclusive,
  };

  struct StreamFormat final
  {
    std::uint32_t sampleRate = 0;
    std::uint8_t channels = 0;
    std::uint8_t bitDepth = 0;
    bool isFloat = false;
    bool isInterleaved = true;

    bool operator==(StreamFormat const&) const = default;
  };

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
    bool isDefault = false;  ///< True if this is the system/backend default

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
    std::optional<StreamFormat> format = std::nullopt;
    bool volumeNotUnity = false;
    bool isMuted = false;
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
   */
  enum class AudioQuality
  {
    Unknown,
    BitPerfect, ///< Exact match, no volume changes, exclusive access
    Lossless,   ///< Safe conversions (e.g. bit-depth upscaling)
    Resampled,  ///< Sample rate conversion occurred
    Mixed,      ///< Multiple active sources sharing the path
    Lossy,      ///< Bit-depth truncation or channel dropping
  };

  /**
   * @brief Represents the current state of a backend and its devices.
   */
  struct BackendSnapshot final
  {
    BackendKind kind = BackendKind::None;
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
