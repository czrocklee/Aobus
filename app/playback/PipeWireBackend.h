// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "IAudioBackend.h"

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

    void open(StreamFormat const& format, AudioRenderCallbacks callbacks) override;
    void start() override;
    void pause() override;
    void resume() override;
    void flush() override;
    void stop() override;
    BackendKind kind() const noexcept override { return BackendKind::PipeWire; }

    // Called from PipeWire process callback
    void process();

  private:
    void destroyResources() noexcept;

    AudioRenderCallbacks _callbacks;
    StreamFormat _format;
    pw_thread_loop* _threadLoop = nullptr;
    pw_context* _context = nullptr;
    pw_stream* _stream = nullptr;
  };

} // namespace app::playback
