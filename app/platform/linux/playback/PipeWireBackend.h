// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "core/playback/IAudioBackend.h"

extern "C"
{
#include <pipewire/core.h>
#include <pipewire/keys.h>
#include <pipewire/link.h>
#include <pipewire/node.h>
#include <pipewire/pipewire.h>
#include <pipewire/proxy.h>
#include <spa/support/loop.h>
#include <spa/utils/hook.h>
}

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace app::playback
{

  class PipeWireBackend final : public app::core::playback::IAudioBackend
  {
  public:
    PipeWireBackend();
    ~PipeWireBackend() override;

    bool open(app::core::playback::StreamFormat const& format,
              app::core::playback::AudioRenderCallbacks callbacks) override;
    void start() override;
    void pause() override;
    void resume() override;
    void flush() override;
    void drain() override;
    void stop() override;
    void close() override;
    app::core::playback::BackendKind kind() const noexcept override
    {
      return app::core::playback::BackendKind::PipeWire;
    }
    app::core::playback::BackendFormatInfo formatInfo() const override;
    std::string_view lastError() const noexcept override { return _lastError; }

    static void onRefreshEvent(void* data, std::uint64_t count);
    static void onCoreDone(void* data, std::uint32_t id, int seq);
    static void onRegistryGlobal(void* data,
                                 std::uint32_t id,
                                 std::uint32_t permissions,
                                 char const* type,
                                 std::uint32_t version,
                                 spa_dict const* props);
    static void onRegistryGlobalRemove(void* data, std::uint32_t id);
    static void onLinkInfo(void* data, pw_link_info const* info);
    static void onNodeInfo(void* data, pw_node_info const* info);
    static void onSinkNodeParam(void* data,
                                int seq,
                                std::uint32_t id,
                                std::uint32_t index,
                                std::uint32_t next,
                                spa_pod const* param);
    static void onStreamParamChanged(void* data, std::uint32_t id, spa_pod const* param);
    static void onStreamStateChanged(void* data, std::int32_t old_state, std::int32_t state, char const* error);
    static void onStreamProcess(void* data);
    static void onStreamDrained(void* data);

    // Called from PipeWire process callback
    void process();
    void handleDrained() noexcept;
    void handleStreamStateChanged(std::int32_t oldState, std::int32_t newState, std::string_view errorMessage);

    // Called from PipeWire core events
    void handleCoreDone(std::uint32_t id, std::int32_t seq);

    // Called from PipeWire registry/node callbacks.
    void handleStreamParamChanged(std::uint32_t id, spa_pod const* param);
    void handleRegistryGlobal(std::uint32_t id, char const* type, std::uint32_t version, spa_dict const* props);
    void handleRegistryGlobalRemove(std::uint32_t id);
    void handleLinkInfo(pw_link_info const* info);
    void handleSinkNodeInfo(pw_node_info const* info);
    void handleSinkNodeParam(std::uint32_t id, spa_pod const* param);

  private:
    // RAII Deleters
    struct PwThreadLoopDeleter final
    {
      void operator()(::pw_thread_loop* p) const noexcept;
    };
    struct PwContextDeleter final
    {
      void operator()(::pw_context* p) const noexcept;
    };
    struct PwStreamDeleter final
    {
      void operator()(::pw_stream* p) const noexcept;
    };

    // Smart Pointers
    using PwThreadLoopPtr = std::unique_ptr<::pw_thread_loop, PwThreadLoopDeleter>;
    using PwContextPtr = std::unique_ptr<::pw_context, PwContextDeleter>;
    using PwStreamPtr = std::unique_ptr<::pw_stream, PwStreamDeleter>;

    struct SpaSourceDeleter final
    {
      SpaSourceDeleter() noexcept = default;
      ::pw_thread_loop* loop = nullptr;
      void operator()(::spa_source* p) const noexcept
      {
        if (p && loop)
        {
          ::pw_loop_destroy_source(::pw_thread_loop_get_loop(loop), p);
        }
      }
    };
    using SpaSourcePtr = std::unique_ptr<::spa_source, SpaSourceDeleter>;

    struct RegistryMonitorState;

    struct LastLoggedState final
    {
      std::uint32_t streamNodeId = 0;
      std::size_t reachableCount = 0;
      std::size_t candidatesCount = 0;
      std::uint32_t desiredSinkId = 0;
      std::string sinkName;
      bool operator==(LastLoggedState const&) const = default;
    };

    void destroyResources() noexcept;
    void setError(std::string message);
    void ensureRegistryMonitor();
    void refreshMonitorState();
    void triggerRefresh();

    app::core::playback::AudioRenderCallbacks _callbacks;
    app::core::playback::StreamFormat _format;
    std::atomic<bool> _drainPending = false;
    mutable std::mutex _infoMutex;
    std::unique_ptr<RegistryMonitorState> _monitorState;
    app::core::playback::BackendFormatInfo _formatInfo;
    LastLoggedState _lastLoggedState;
    std::string _lastError;

    // RAII-wrapped PipeWire handles - ordered for correct destruction
    PwThreadLoopPtr _threadLoop;
    PwContextPtr _context;
    PwStreamPtr _stream;
    SpaSourcePtr _refreshEvent;
  };

} // namespace app::playback
