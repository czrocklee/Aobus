// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "core/AudioFormat.h"

extern "C"
{
#include <pipewire/pipewire.h>
#include <spa/param/audio/raw.h>
}

#include <memory>
#include <optional>
#include <string>
#include <cstring>

namespace app::playback::detail
{

  // --- RAII Deleters (inline implementations) ---

  struct PwProxyDeleter final { void operator()(void* p) const noexcept { ::pw_proxy_destroy(static_cast<::pw_proxy*>(p)); } };
  template<typename T> using PwProxyPtr = std::unique_ptr<T, PwProxyDeleter>;

  struct PwThreadLoopDeleter final { void operator()(::pw_thread_loop* p) const noexcept { ::pw_thread_loop_destroy(p); } };
  using PwThreadLoopPtr = std::unique_ptr<::pw_thread_loop, PwThreadLoopDeleter>;

  struct PwContextDeleter final { void operator()(::pw_context* p) const noexcept { ::pw_context_destroy(p); } };
  using PwContextPtr = std::unique_ptr<::pw_context, PwContextDeleter>;

  struct PwCoreDeleter final { void operator()(::pw_core* p) const noexcept { ::pw_core_disconnect(p); } };
  using PwCorePtr = std::unique_ptr<::pw_core, PwCoreDeleter>;

  struct PwStreamDeleter final { void operator()(::pw_stream* p) const noexcept { ::pw_stream_destroy(p); } };
  using PwStreamPtr = std::unique_ptr<::pw_stream, PwStreamDeleter>;

  struct PwRegistryDeleter final { void operator()(::pw_registry* p) const noexcept { ::pw_proxy_destroy(reinterpret_cast<::pw_proxy*>(p)); } };
  using PwRegistryPtr = std::unique_ptr<::pw_registry, PwRegistryDeleter>;

  struct PwLinkDeleter final { void operator()(::pw_link* p) const noexcept { ::pw_proxy_destroy(reinterpret_cast<::pw_proxy*>(p)); } };
  using PwLinkPtr = std::unique_ptr<::pw_link, PwLinkDeleter>;

  struct SpaSourceDeleter final
  {
    ::pw_thread_loop* loop = nullptr;
    void operator()(::spa_source* p) const noexcept
    {
      if (p && loop) ::pw_loop_destroy_source(::pw_thread_loop_get_loop(loop), p);
    }
  };
  using SpaSourcePtr = std::unique_ptr<::spa_source, SpaSourceDeleter>;

  class SpaHookGuard final
  {
  public:
    SpaHookGuard() noexcept { std::memset(&_hook, 0, sizeof(_hook)); }
    ~SpaHookGuard() { reset(); }
    SpaHookGuard(SpaHookGuard const&) = delete;
    SpaHookGuard& operator=(SpaHookGuard const&) = delete;
    void reset() noexcept
    {
      if (_hook.link.next != nullptr) ::spa_hook_remove(&_hook);
      std::memset(&_hook, 0, sizeof(_hook));
    }
    ::spa_hook* get() noexcept { return &_hook; }
  private:
    ::spa_hook _hook;
  };

  // --- Shared Helper Functions ---

  void ensurePipeWireInit();
  std::optional<std::uint32_t> parseUintProperty(char const* value);
  std::optional<app::core::AudioFormat> parseRawStreamFormat(::spa_pod const* param);

} // namespace app::playback::detail
