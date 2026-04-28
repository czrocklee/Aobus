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
  // --- RAII Deleters & Smart Pointers ---

  struct PwProxyDeleter final { void operator()(void* p) const noexcept; };
  template<typename T> using PwProxyPtr = std::unique_ptr<T, PwProxyDeleter>;

  struct PwThreadLoopDeleter final { void operator()(::pw_thread_loop* p) const noexcept; };
  using PwThreadLoopPtr = std::unique_ptr<::pw_thread_loop, PwThreadLoopDeleter>;

  struct PwContextDeleter final { void operator()(::pw_context* p) const noexcept; };
  using PwContextPtr = std::unique_ptr<::pw_context, PwContextDeleter>;

  struct PwCoreDeleter final { void operator()(::pw_core* p) const noexcept; };
  using PwCorePtr = std::unique_ptr<::pw_core, PwCoreDeleter>;

  struct PwStreamDeleter final { void operator()(::pw_stream* p) const noexcept; };
  using PwStreamPtr = std::unique_ptr<::pw_stream, PwStreamDeleter>;

  struct PwRegistryDeleter final { void operator()(::pw_registry* p) const noexcept; };
  using PwRegistryPtr = std::unique_ptr<::pw_registry, PwRegistryDeleter>;

  struct PwLinkDeleter final { void operator()(::pw_link* p) const noexcept; };
  using PwLinkPtr = std::unique_ptr<::pw_link, PwLinkDeleter>;

  struct SpaSourceDeleter final
  {
    ::pw_thread_loop* loop = nullptr;
    void operator()(::spa_source* p) const noexcept;
  };
  using SpaSourcePtr = std::unique_ptr<::spa_source, SpaSourceDeleter>;

  class SpaHookGuard final
  {
  public:
    SpaHookGuard() noexcept;
    ~SpaHookGuard();
    SpaHookGuard(SpaHookGuard const&) = delete;
    SpaHookGuard& operator=(SpaHookGuard const&) = delete;
    void reset() noexcept;
    ::spa_hook* get() noexcept { return &_hook; }
  private:
    ::spa_hook _hook;
  };

  // --- Internal Structs ---

  struct NodeRecord final
  {
    std::uint32_t version = 0;
    std::string mediaClass;
    std::string nodeName;
    std::string nodeNick;
    std::string nodeDescription;
    std::string objectPath;
    std::optional<std::uint32_t> objectSerial;
    std::optional<std::uint32_t> driverId;
  };

  struct LinkRecord final
  {
    std::uint32_t outputNodeId = PW_ID_ANY;
    std::uint32_t inputNodeId = PW_ID_ANY;
    ::pw_link_state state = PW_LINK_STATE_INIT;
  };

  struct SinkProps final
  {
    std::optional<float> volume;
    std::optional<bool> mute;
    std::vector<float> channelVolumes;
    std::optional<bool> softMute;
    std::vector<float> softVolumes;
  };

  // --- Helpers ---

  void ensurePipeWireInit();
  bool isSinkMediaClass(std::string const& mediaClass);
  bool isActiveLink(::pw_link_state state) noexcept;
  std::optional<std::uint32_t> parseUintProperty(char const* value);
  std::string lookupProperty(::spa_dict const* props, char const* key);
  std::string formatStreamFormat(app::core::AudioFormat const& format);
  NodeRecord parseNodeRecord(std::uint32_t version, ::spa_dict const* props);
  std::optional<app::core::AudioFormat> parseRawStreamFormat(::spa_pod const* param);
  bool copyFloatArray(::spa_pod const& pod, std::vector<float>& output);
  void mergeSinkProps(SinkProps& sinkProps, ::spa_pod const* param);

  // --- PipeWireMonitor ---

  class PipeWireMonitor final
  {
  public:
    struct LinkBinding final
    {
      std::uint32_t id = PW_ID_ANY;
      std::uint32_t outputId = PW_ID_ANY;
      std::uint32_t inputId = PW_ID_ANY;
    };

    explicit PipeWireMonitor(::pw_thread_loop* loop, ::pw_core* core, app::core::backend::AudioRenderCallbacks callbacks);
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

    std::uint64_t subscribeGraph(std::string_view routeAnchor, std::function<void(app::core::backend::AudioGraph const&)> callback);
    void unsubscribeGraph(std::uint64_t id);

    // Callbacks (must be public for C-style interop)
    static void onCoreDone(void* data, std::uint32_t id, int seq);
    static void onRegistryGlobal(void* data, std::uint32_t id, std::uint32_t permissions, char const* type, std::uint32_t version, ::spa_dict const* props);
    static void onRegistryGlobalRemove(void* data, std::uint32_t id);
    static void onNodeInfo(void* data, ::pw_node_info const* info);
    static void onNodeParam(void* data, int seq, std::uint32_t id, std::uint32_t index, std::uint32_t next, ::spa_pod const* param);
    static void onLinkInfo(void* data, ::pw_link_info const* info);

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
  };

} // namespace app::playback
