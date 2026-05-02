// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <ao/audio/IBackend.h>
#include <ao/audio/IBackendProvider.h>

#include <memory>
#include <vector>

namespace ao::audio
{
  /**
   * @brief A backend that does nothing. Used as a fallback or for testing.
   */
  class NullBackend final : public IBackend
  {
  public:
    class NullManager final : public IBackendProvider
    {
    public:
      Subscription subscribeDevices(OnDevicesChangedCallback callback) override
      {
        if (callback)
        {
          callback({{.id = "null",
                     .displayName = "None",
                     .description = "No audio output",
                     .backendKind = BackendKind::None,
                     .capabilities = {}}});
        }
        return Subscription{};
      }

      std::unique_ptr<IBackend> createBackend(Device const& /*device*/) override
      {
        return std::make_unique<NullBackend>();
      }

      Subscription subscribeGraph(std::string_view /*routeAnchor*/, OnGraphChangedCallback callback) override
      {
        if (callback)
        {
          flow::Graph graph;
          graph.nodes.push_back(
            {.id = "null-stream", .type = flow::NodeType::Stream, .name = "Null Stream", .objectPath = ""});
          graph.nodes.push_back(
            {.id = "null-sink", .type = flow::NodeType::Sink, .name = "Null Device", .objectPath = ""});
          graph.connections.push_back({.sourceId = "null-stream", .destId = "null-sink", .isActive = true});
          callback(graph);
        }
        return Subscription{};
      }
    };

    NullBackend() = default;
    ~NullBackend() override = default;

    ao::Result<> open(Format const& /*format*/, RenderCallbacks /*callbacks*/) override { return {}; }

    void reset() override {}

    void start() override {}
    void pause() override {}
    void resume() override {}
    void flush() override {}
    void drain() override
    {
      // No-op, but we should probably signal completion if requested
    }
    void stop() override {}
    void close() override {}

    void setExclusiveMode(bool /*exclusive*/) override {}
    bool isExclusiveMode() const noexcept override { return false; }

    BackendKind kind() const noexcept override { return BackendKind::None; }
  };
} // namespace ao::audio
