// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "core/backend/IAudioBackend.h"
#include "core/backend/IBackendManager.h"

#include <memory>
#include <vector>

namespace app::core::backend
{

  /**
   * @brief A backend that does nothing. Used as a fallback or for testing.
   */
  class NullBackend final : public IAudioBackend
  {
  public:
    class NullManager final : public IBackendManager
    {
    public:
      void setDevicesChangedCallback(OnDevicesChangedCallback /*callback*/) override {}
      std::vector<AudioDevice> enumerateDevices() override
      {
        return {{.id = "null",
                 .displayName = "None",
                 .description = "No audio output",
                 .backendKind = BackendKind::None,
                 .capabilities = {}}};
      }

      std::unique_ptr<IAudioBackend> createBackend(AudioDevice const& /*device*/) override
      {
        return std::make_unique<NullBackend>();
      }

      struct NullSubscription final : public IGraphSubscription
      {};

      std::unique_ptr<IGraphSubscription> subscribeGraph(std::string_view /*routeAnchor*/,
                                                         OnGraphChangedCallback callback) override
      {
        if (callback)
        {
          AudioGraph graph;
          graph.nodes.push_back(
            {.id = "null-stream", .type = AudioNodeType::Stream, .name = "Null Stream", .objectPath = ""});
          graph.nodes.push_back(
            {.id = "null-sink", .type = AudioNodeType::Sink, .name = "Null Device", .objectPath = ""});
          graph.links.push_back({.sourceId = "null-stream", .destId = "null-sink", .isActive = true});
          callback(graph);
        }
        return std::make_unique<NullSubscription>();
      }
    };

    NullBackend() = default;
    ~NullBackend() override = default;

    rs::Result<> open(AudioFormat const& /*format*/, AudioRenderCallbacks /*callbacks*/) override
    {
      return rs::Result<>();
    }

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

} // namespace app::core::backend
