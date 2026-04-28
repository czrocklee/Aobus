// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "core/AudioFormat.h"
#include "core/backend/BackendTypes.h"
#include "core/backend/IAudioBackend.h"

extern "C"
{
#include <pipewire/pipewire.h>
#include <spa/param/audio/raw.h>
}

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace app::playback
{

  class PipeWireMonitor final
  {
  public:
    explicit PipeWireMonitor(::pw_thread_loop* loop,
                             ::pw_core* core,
                             app::core::backend::AudioRenderCallbacks callbacks);
    ~PipeWireMonitor();

    void start();
    void stop();
    void refresh();

    void setStream(::pw_stream* stream);
    void setCallbacks(app::core::backend::AudioRenderCallbacks callbacks);
    void setNegotiatedFormat(std::optional<app::core::AudioFormat> format);

    void setDevicesChangedCallback(std::function<void()> callback);
    std::vector<app::core::backend::AudioDevice> enumerateSinks() const;
    std::optional<std::uint32_t> findSinkIdByName(std::string_view name) const;

    std::uint64_t subscribeGraph(std::string_view routeAnchor,
                                 std::function<void(app::core::backend::AudioGraph const&)> callback);
    void unsubscribeGraph(std::uint64_t id);

    // PipeWire event callbacks (public for C-style interop)
    static void onCoreDone(void* data, std::uint32_t id, int seq);
    static void onRegistryGlobal(void* data,
                                 std::uint32_t id,
                                 std::uint32_t permissions,
                                 char const* type,
                                 std::uint32_t version,
                                 ::spa_dict const* props);
    static void onRegistryGlobalRemove(void* data, std::uint32_t id);
    static void onNodeInfo(void* data, ::pw_node_info const* info);
    static void onNodeParam(void* data,
                            int seq,
                            std::uint32_t id,
                            std::uint32_t index,
                            std::uint32_t next,
                            ::spa_pod const* param);
    static void onLinkInfo(void* data, ::pw_link_info const* info);

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
  };

} // namespace app::playback
