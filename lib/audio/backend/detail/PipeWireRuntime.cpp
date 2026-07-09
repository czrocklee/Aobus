// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/backend/detail/PipeWireRuntime.h>

extern "C"
{
#include <pipewire/pipewire.h>
}

#include <cstdint>
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
