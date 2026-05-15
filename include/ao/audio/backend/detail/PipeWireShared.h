// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/audio/Format.h>

extern "C"
{
#include <pipewire/pipewire.h>
#include <spa/param/audio/raw.h>
}

#include <cstring>
#include <memory>
#include <optional>
#include <string>

namespace ao::audio::backend::detail
{
  // --- RAII Deleters (inline implementations) ---
  struct PwProxyDeleter final
  {
    void operator()(void* ptr) const noexcept { ::pw_proxy_destroy(static_cast<::pw_proxy*>(ptr)); }
  };

  template<typename T>
  using PwProxyPtr = std::unique_ptr<T, PwProxyDeleter>;

  struct PwThreadLoopDeleter final
  {
    void operator()(::pw_thread_loop* ptr) const noexcept { ::pw_thread_loop_destroy(ptr); }
  };

  using PwThreadLoopPtr = std::unique_ptr<::pw_thread_loop, PwThreadLoopDeleter>;

  struct PwContextDeleter final
  {
    void operator()(::pw_context* ptr) const noexcept { ::pw_context_destroy(ptr); }
  };

  using PwContextPtr = std::unique_ptr<::pw_context, PwContextDeleter>;

  struct PwCoreDeleter final
  {
    void operator()(::pw_core* ptr) const noexcept { ::pw_core_disconnect(ptr); }
  };

  using PwCorePtr = std::unique_ptr<::pw_core, PwCoreDeleter>;

  struct PwStreamDeleter final
  {
    void operator()(::pw_stream* ptr) const noexcept { ::pw_stream_destroy(ptr); }
  };

  using PwStreamPtr = std::unique_ptr<::pw_stream, PwStreamDeleter>;

  using PwRegistryPtr = PwProxyPtr<::pw_registry>;
  using PwLinkPtr = PwProxyPtr<::pw_link>;

  struct SpaSourceDeleter final
  {
    ::pw_thread_loop* loop = nullptr;

    void operator()(::spa_source* ptr) const noexcept
    {
        ::pw_loop_destroy_source(::pw_thread_loop_get_loop(loop), ptr);
    }
  };

  using SpaSourcePtr = std::unique_ptr<::spa_source, SpaSourceDeleter>;

  class SpaHookGuard final
  {
  public:
    SpaHookGuard() noexcept : _hook{} {}
    ~SpaHookGuard() noexcept { reset(); }

    SpaHookGuard(SpaHookGuard const&) = delete;
    SpaHookGuard& operator=(SpaHookGuard const&) = delete;

    SpaHookGuard(SpaHookGuard&&) = delete;
    SpaHookGuard& operator=(SpaHookGuard&&) = delete;

    void reset() noexcept
    {
      if (_hook.link.next != nullptr)
      {
        ::spa_hook_remove(&_hook);
      }

      std::memset(&_hook, 0, sizeof(_hook));
    }
    
    ::spa_hook* get() noexcept { return &_hook; }

  private:
    ::spa_hook _hook;
  };

  // --- Shared Helper Functions ---

  void ensurePipeWireInit();
  std::optional<std::uint32_t> parseUintProperty(char const* value) noexcept;
  std::optional<Format> parseRawStreamFormat(::spa_pod const* param) noexcept;
} // namespace ao::audio::backend::detail
