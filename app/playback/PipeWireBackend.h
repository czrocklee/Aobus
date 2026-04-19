// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "IAudioBackend.h"

#include <string>
#include <string_view>

struct pw_context;
struct pw_stream;
struct pw_thread_loop;
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
    std::string_view lastError() const noexcept override { return _lastError; }

    // Called from PipeWire process callback
    void process();
    void handleDrained() noexcept;

  private:
    void destroyResources() noexcept;
    void setError(std::string message);

    AudioRenderCallbacks _callbacks;
    StreamFormat _format;
    pw_thread_loop* _threadLoop = nullptr;
    pw_context* _context = nullptr;
    pw_stream* _stream = nullptr;
    bool _drainPending = false;
    std::string _lastError;
  };

} // namespace app::playback
