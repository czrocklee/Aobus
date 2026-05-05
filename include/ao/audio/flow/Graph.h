// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/audio/Format.h>

#include <optional>
#include <string>
#include <vector>

namespace ao::audio::flow
{
  /**
   * @brief Type of an audio processing node in the playback graph.
   */
  enum class NodeType
  {
    Decoder,
    Engine,
    Stream,
    Intermediary,
    Sink,
    ExternalSource,
  };

  /**
   * @brief Represents a semantic component in the audio pipeline.
   */
  struct Node final
  {
    std::string id = "";
    NodeType type = NodeType::Intermediary;
    std::string name = "";
    std::optional<Format> optFormat = std::nullopt;
    bool volumeNotUnity = false;
    bool isMuted = false;
    bool isLossySource = false;
    std::string objectPath = "";

    bool operator==(Node const&) const = default;
  };

  /**
   * @brief Represents a connection between two audio nodes.
   */
  struct Connection final
  {
    std::string sourceId = "";
    std::string destId = "";
    bool isActive = true;

    bool operator==(Connection const&) const = default;
  };

  /**
   * @brief Topological representation of the entire playback path.
   */
  struct Graph final
  {
    std::vector<Node> nodes;
    std::vector<Connection> connections;

    bool operator==(Graph const&) const = default;
  };
} // namespace ao::audio::flow
