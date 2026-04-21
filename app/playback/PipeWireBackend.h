// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "IAudioBackend.h"

#include <memory>
#include <mutex>
#include <string>
#include <string_view>

struct pw_context;
struct pw_link_info;
struct pw_node_info;
struct pw_registry;
struct pw_stream_events;
struct pw_stream;
struct pw_thread_loop;
struct spa_dict;
struct spa_pod;

namespace app::playback
{

  class PipeWireBackend final : public IAudioBackend
  {
  public:
    PipeWireBackend();
    ~PipeWireBackend() override;

    bool open(StreamFormat const& format, AudioRenderCallbacks callbacks) override;
    void start() override;
    void pause() override;
    void resume() override;
    void flush() override;
    void drain() override;
    void stop() override;
    void close() override;
    BackendKind kind() const noexcept override { return BackendKind::PipeWire; }
    BackendFormatInfo formatInfo() const override;
    std::string_view lastError() const noexcept override { return _lastError; }

    // Called from PipeWire process callback
    void process();
    void handleDrained() noexcept;
    void handleStreamStateChanged(int oldState, int newState, char const* errorMessage);

    // Called from PipeWire core events
    void handleCoreDone(std::uint32_t id, int seq);

    // Called from PipeWire registry/node callbacks.
    void handleStreamParamChanged(std::uint32_t id, spa_pod const* param);
    void handleRegistryGlobal(std::uint32_t id,
                              char const* type,
                              std::uint32_t version,
                              spa_dict const* props);
    void handleRegistryGlobalRemove(std::uint32_t id);
    void handleLinkInfo(pw_link_info const* info);
    void handleSinkNodeInfo(pw_node_info const* info);
    void handleSinkNodeParam(std::uint32_t id, spa_pod const* param);

  private:
    struct RegistryMonitorState;

    void destroyResources() noexcept;
    void setError(std::string message);
    void ensureRegistryMonitor();
    void refreshMonitorState();

    AudioRenderCallbacks _callbacks;
    StreamFormat _format;
    pw_thread_loop* _threadLoop = nullptr;
    pw_context* _context = nullptr;
    pw_stream* _stream = nullptr;
    bool _drainPending = false;
    mutable std::mutex _infoMutex;
    std::unique_ptr<RegistryMonitorState> _monitorState;
    BackendFormatInfo _formatInfo;
    std::string _lastError;
  };

} // namespace app::playback
