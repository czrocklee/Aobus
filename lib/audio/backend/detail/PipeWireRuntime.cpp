// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "backend/detail/PipeWireRuntime.h"

extern "C"
{
#include <pipewire/pipewire.h>
#include <spa/utils/hook.h>
}

#include <cstdint>
#include <cstring>
#include <mutex>

namespace ao::audio::backend::detail
{
  namespace
  {
    struct PipeWireRuntimeState final
    {
      std::mutex mutex;
      std::uint32_t refCount = 0;
    };

    PipeWireRuntimeState& pipeWireRuntimeState()
    {
      static PipeWireRuntimeState state;
      return state;
    }
  } // namespace

  void SpaHookGuard::reset() noexcept
  {
    if (_hook.link.next != nullptr)
    {
      ::spa_hook_remove(&_hook);
    }

    std::memset(&_hook, 0, sizeof(_hook));
  }

  PwThreadLoopGuard::PwThreadLoopGuard(::pw_thread_loop* loop) noexcept
    : _loop{loop}
  {
    if (_loop != nullptr)
    {
      ::pw_thread_loop_lock(_loop);
    }
  }

  PwThreadLoopGuard::~PwThreadLoopGuard() noexcept
  {
    if (_loop != nullptr)
    {
      ::pw_thread_loop_unlock(_loop);
    }
  }

  PipeWireEnvironmentGuard::PipeWireEnvironmentGuard()
  {
    auto& state = pipeWireRuntimeState();
    auto const lock = std::scoped_lock{state.mutex};

    if (state.refCount == 0)
    {
      ::pw_init(nullptr, nullptr);
    }

    ++state.refCount;
    _active = true;
  }

  PipeWireEnvironmentGuard::~PipeWireEnvironmentGuard() noexcept
  {
    if (!_active)
    {
      return;
    }

    auto& state = pipeWireRuntimeState();
    auto const lock = std::scoped_lock{state.mutex};
    --state.refCount;

    if (state.refCount == 0)
    {
      ::pw_deinit();
    }
  }
} // namespace ao::audio::backend::detail
