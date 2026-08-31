// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "CoreAudioProvider.h"

#include "CoreAudioBackend.h"
#include "detail/BackendDeviceRegistry.h"
#include "detail/BackendGraphRegistry.h"
#include "detail/CallbackFence.h"
#include "detail/CoreAudioDeviceDiscovery.h"
#include "detail/CoreAudioProviderMonitorHooks.h"
#include <ao/Contract.h>
#include <ao/audio/Backend.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/BackendProvider.h>
#include <ao/audio/Device.h>
#include <ao/audio/Subscription.h>
#include <ao/audio/flow/Graph.h>
#include <ao/utility/CallbackStackScope.h>
#include <ao/utility/ThreadName.h>

#include <CoreAudio/AudioHardware.h>
#include <CoreAudio/AudioHardwareBase.h>
#include <MacTypes.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <semaphore>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace ao::audio::backend
{
  namespace
  {
    constexpr auto kChangeCoalesceDelay = std::chrono::milliseconds{150};

    void invokeHook(std::function<void()> const& hook, char const* const context) noexcept
    {
      if (!hook)
      {
        return;
      }

      try
      {
        hook();
      }
      catch (...)
      {
        AO_FATAL_EXCEPTION(std::current_exception(), context);
      }
    }

    class CallbackLeave final
    {
    public:
      explicit CallbackLeave(detail::CallbackFence& fence) noexcept
        : _fence{fence}
      {
      }

      ~CallbackLeave() { _fence.leave(); }

      CallbackLeave(CallbackLeave const&) = delete;
      CallbackLeave& operator=(CallbackLeave const&) = delete;
      CallbackLeave(CallbackLeave&&) = delete;
      CallbackLeave& operator=(CallbackLeave&&) = delete;

    private:
      detail::CallbackFence& _fence;
    };

    struct CoreAudioMonitorState final
    {
      std::shared_ptr<detail::BackendDeviceRegistry> deviceRegistryPtr;
      std::shared_ptr<detail::CoreAudioProviderMonitorHooks> monitorHooksPtr;
      detail::CallbackFence listenerFence{};
      std::counting_semaphore<> changeSignal{0};
      std::atomic<bool> shutdownRequested{false};
      std::array<::AudioObjectPropertyAddress, 2> addresses{};
      std::size_t registeredAddressCount = 0U;

      CoreAudioMonitorState(std::shared_ptr<detail::BackendDeviceRegistry> registryPtr,
                            std::shared_ptr<detail::CoreAudioProviderMonitorHooks> hooksPtr)
        : deviceRegistryPtr{std::move(registryPtr)}, monitorHooksPtr{std::move(hooksPtr)}
      {
        addresses = {::AudioObjectPropertyAddress{.mSelector = ::kAudioHardwarePropertyDevices,
                                                  .mScope = ::kAudioObjectPropertyScopeGlobal,
                                                  .mElement = ::kAudioObjectPropertyElementMain},
                     ::AudioObjectPropertyAddress{.mSelector = ::kAudioHardwarePropertyDefaultOutputDevice,
                                                  .mScope = ::kAudioObjectPropertyScopeGlobal,
                                                  .mElement = ::kAudioObjectPropertyElementMain}};
        deviceRegistryPtr->publish(enumerateDevices());
      }

      ~CoreAudioMonitorState()
      {
        requestShutdown();

        if (monitorHooksPtr)
        {
          invokeHook(monitorHooksPtr->onMonitorStateDestroyed, "Core Audio monitor destruction observer");
        }
      }

      CoreAudioMonitorState(CoreAudioMonitorState const&) = delete;
      CoreAudioMonitorState& operator=(CoreAudioMonitorState const&) = delete;
      CoreAudioMonitorState(CoreAudioMonitorState&&) = delete;
      CoreAudioMonitorState& operator=(CoreAudioMonitorState&&) = delete;

      std::vector<Device> enumerateDevices() const
      {
        return monitorHooksPtr && monitorHooksPtr->enumerateDevices ? monitorHooksPtr->enumerateDevices()
                                                                    : detail::enumerateCoreAudioOutputDevices();
      }

      static ::OSStatus propertyChanged(::AudioObjectID /*objectId*/,
                                        ::UInt32 /*addressCount*/,
                                        ::AudioObjectPropertyAddress const* /*addresses*/,
                                        void* const context) noexcept
      {
        auto& state = *static_cast<CoreAudioMonitorState*>(context);

        if (!state.listenerFence.tryEnter())
        {
          return ::noErr;
        }

        auto const leave = CallbackLeave{state.listenerFence};
        state.changeSignal.release();
        return ::noErr;
      }

      bool installNativeListeners()
      {
        if (monitorHooksPtr && monitorHooksPtr->enumerateDevices)
        {
          return true;
        }

        listenerFence.open();
        auto const installed =
          std::ranges::all_of(addresses,
                              [this](auto& address)
                              {
                                if (::AudioObjectAddPropertyListener(
                                      ::kAudioObjectSystemObject, &address, &propertyChanged, this) != ::noErr)
                                {
                                  return false;
                                }

                                ++registeredAddressCount;
                                return true;
                              });

        if (!installed)
        {
          requestShutdown();
        }

        return installed;
      }

      void monitorLoop()
      {
        while (!shutdownRequested.load(std::memory_order_acquire))
        {
          changeSignal.acquire();

          if (shutdownRequested.load(std::memory_order_acquire))
          {
            return;
          }

          auto const coalesceDeadline = std::chrono::steady_clock::now() + kChangeCoalesceDelay;

          while (changeSignal.try_acquire_until(coalesceDeadline))
          {
            if (shutdownRequested.load(std::memory_order_acquire))
            {
              return;
            }
          }

          deviceRegistryPtr->publish(enumerateDevices());

          if (monitorHooksPtr)
          {
            invokeHook(monitorHooksPtr->onRefreshComplete, "Core Audio monitor refresh observer");
          }
        }
      }

      void requestRefresh() noexcept
      {
        if (!shutdownRequested.load(std::memory_order_acquire))
        {
          changeSignal.release();
        }
      }

      void requestShutdown() noexcept
      {
        if (shutdownRequested.exchange(true, std::memory_order_acq_rel))
        {
          return;
        }

        listenerFence.close();

        while (registeredAddressCount != 0U)
        {
          --registeredAddressCount;
          auto& address = addresses[registeredAddressCount];
          ::AudioObjectRemovePropertyListener(::kAudioObjectSystemObject, &address, &propertyChanged, this);
        }

        listenerFence.wait();
        changeSignal.release();
      }
    };

    class CoreAudioProviderControl final : public std::enable_shared_from_this<CoreAudioProviderControl>
    {
    public:
      explicit CoreAudioProviderControl(std::shared_ptr<detail::CoreAudioProviderMonitorHooks> monitorHooksPtr)
        : _deviceRegistryPtr{std::make_shared<detail::BackendDeviceRegistry>()}
        , _graphRegistryPtr{std::make_shared<detail::BackendGraphRegistry>()}
        , _monitorStatePtr{std::make_shared<CoreAudioMonitorState>(_deviceRegistryPtr, std::move(monitorHooksPtr))}
      {
      }

      ~CoreAudioProviderControl() { shutdown(); }

      CoreAudioProviderControl(CoreAudioProviderControl const&) = delete;
      CoreAudioProviderControl& operator=(CoreAudioProviderControl const&) = delete;
      CoreAudioProviderControl(CoreAudioProviderControl&&) = delete;
      CoreAudioProviderControl& operator=(CoreAudioProviderControl&&) = delete;

      void startMonitor()
      {
        auto const statePtr = _monitorStatePtr;

        if (statePtr->monitorHooksPtr)
        {
          statePtr->monitorHooksPtr->requestRefresh = [weakStatePtr = std::weak_ptr{statePtr}]
          {
            if (auto const retainedStatePtr = weakStatePtr.lock(); retainedStatePtr)
            {
              retainedStatePtr->requestRefresh();
            }
          };
        }

        if (!statePtr->installNativeListeners())
        {
          auto const lock = std::scoped_lock{_lifecycleMutex};
          _monitorExited = true;
          return;
        }

        try
        {
          statePtr->deviceRegistryPtr->publish(statePtr->enumerateDevices());
          auto const retainedControlPtr = shared_from_this();
          _monitorThread = std::jthread{
            [retainedControlPtr, statePtr]
            {
              auto const monitorScope = utility::CallbackStackScope{retainedControlPtr->monitorScopeIdentity()};

              try
              {
                setCurrentThreadName("CoreAudioMonitor");
                statePtr->monitorLoop();

                if (statePtr->monitorHooksPtr)
                {
                  invokeHook(statePtr->monitorHooksPtr->onMonitorExit, "Core Audio monitor-exit observer");
                }
              }
              catch (...)
              {
                AO_FATAL_EXCEPTION(std::current_exception(), "Core Audio device-monitor thread");
              }

              retainedControlPtr->markMonitorExited();
            }};
        }
        catch (...)
        {
          statePtr->requestShutdown();
          markMonitorExited();
          throw;
        }
      }

      void shutdown() noexcept
      {
        bool returnWithoutWaiting = false;
        bool startedShutdown = false;

        {
          auto lock = std::unique_lock{_lifecycleMutex};
          returnWithoutWaiting = isCurrentCallbackOrMonitor();

          if (_lifecycle == Lifecycle::Stopped)
          {
            return;
          }

          if (_lifecycle == Lifecycle::Stopping)
          {
            if (returnWithoutWaiting)
            {
              return;
            }

            lock.unlock();
            notifyShutdownWait();
            lock.lock();
            _completionChanged.wait(lock, [this] { return _lifecycle == Lifecycle::Stopped; });
            return;
          }

          _lifecycle = Lifecycle::Stopping;
          startedShutdown = true;
        }

        if (startedShutdown)
        {
          notifyShutdownStarted();
        }

        auto const monitorStatePtr = _monitorStatePtr;
        auto const graphRegistryPtr = _graphRegistryPtr;
        auto const deviceRegistryPtr = _deviceRegistryPtr;
        monitorStatePtr->requestShutdown();
        graphRegistryPtr->shutdown();
        deviceRegistryPtr->shutdown();

        if (_monitorThread.joinable())
        {
          if (returnWithoutWaiting)
          {
            // The worker captures only independent provider control and monitor
            // state. Detaching lets the current outward callback unwind before
            // shared completion observes callback and monitor quiescence.
            _monitorThread.detach();
          }
          else
          {
            _monitorThread.join();
          }
        }

        {
          auto lock = std::unique_lock{_lifecycleMutex};
          _listenerRetired = true;
          _registriesRetired = true;
          _threadRetired = true;
          updateCompletionLocked();

          if (returnWithoutWaiting)
          {
            return;
          }

          _completionChanged.wait(lock, [this] { return _lifecycle == Lifecycle::Stopped; });
        }
      }

      Subscription subscribeDevices(BackendProvider::OnDevicesChangedCallback callback)
      {
        if (!callback || !acceptsSubscriptions())
        {
          return {};
        }

        auto const weakControlPtr = weak_from_this();
        auto const deviceRegistryPtr = _deviceRegistryPtr;
        auto sub = deviceRegistryPtr->subscribe(
          [weakControlPtr, callback = std::move(callback)](std::vector<Device> const& devices)
          {
            if (auto const controlPtr = weakControlPtr.lock(); controlPtr)
            {
              controlPtr->invokeDeviceCallback(callback, devices);
            }
          });

        if (!acceptsSubscriptions())
        {
          sub.reset();
          return {};
        }

        return sub;
      }

      Subscription subscribeGraph(std::string_view const routeAnchor, BackendProvider::OnGraphChangedCallback callback)
      {
        if (!callback || !acceptsSubscriptions())
        {
          return {};
        }

        auto const weakControlPtr = weak_from_this();
        auto const graphRegistryPtr = _graphRegistryPtr;
        auto sub =
          graphRegistryPtr->subscribe(routeAnchor,
                                      [weakControlPtr, callback = std::move(callback)](flow::Graph const& graph)
                                      {
                                        if (auto const controlPtr = weakControlPtr.lock(); controlPtr)
                                        {
                                          controlPtr->invokeGraphCallback(callback, graph);
                                        }
                                      });

        if (!acceptsSubscriptions())
        {
          sub.reset();
          return {};
        }

        return sub;
      }

      std::vector<Device> devices() const { return _deviceRegistryPtr->snapshot(); }

      std::shared_ptr<detail::BackendGraphRegistry> graphRegistry() const { return _graphRegistryPtr; }

    private:
      enum class Lifecycle : std::uint8_t
      {
        Running,
        Stopping,
        Stopped,
      };

      class [[nodiscard]] CallbackScope final
      {
      public:
        explicit CallbackScope(CoreAudioProviderControl& control)
          : _control{control}
        {
          auto const lock = std::scoped_lock{_control._lifecycleMutex};

          auto const callbackAlreadyActive = utility::CallbackStackScope::containsIdentity(&_control);

          if (_control._lifecycle == Lifecycle::Running ||
              (_control._lifecycle == Lifecycle::Stopping && !callbackAlreadyActive))
          {
            ++_control._activeCallbackCount;
            _optCallbackStackScope.emplace(&_control);
          }
        }

        ~CallbackScope()
        {
          if (!_optCallbackStackScope)
          {
            return;
          }

          _optCallbackStackScope.reset();
          auto const lock = std::scoped_lock{_control._lifecycleMutex};
          --_control._activeCallbackCount;
          _control.updateCompletionLocked();
        }

        CallbackScope(CallbackScope const&) = delete;
        CallbackScope& operator=(CallbackScope const&) = delete;
        CallbackScope(CallbackScope&&) = delete;
        CallbackScope& operator=(CallbackScope&&) = delete;

        bool isAdmitted() const noexcept { return _optCallbackStackScope.has_value(); }

      private:
        CoreAudioProviderControl& _control;
        std::optional<utility::CallbackStackScope> _optCallbackStackScope;
      };

      bool acceptsSubscriptions() const
      {
        auto const lock = std::scoped_lock{_lifecycleMutex};
        return _lifecycle == Lifecycle::Running;
      }

      void const* monitorScopeIdentity() const noexcept { return &_monitorScopeIdentity; }

      bool isCurrentCallbackOrMonitor() const noexcept
      {
        return utility::CallbackStackScope::containsIdentity(this) ||
               utility::CallbackStackScope::containsIdentity(monitorScopeIdentity());
      }

      void invokeDeviceCallback(BackendProvider::OnDevicesChangedCallback const& callback,
                                std::vector<Device> const& devices)
      {
        auto const scope = CallbackScope{*this};

        if (scope.isAdmitted())
        {
          callback(devices);
        }
      }

      void invokeGraphCallback(BackendProvider::OnGraphChangedCallback const& callback, flow::Graph const& graph)
      {
        auto const scope = CallbackScope{*this};

        if (scope.isAdmitted())
        {
          callback(graph);
        }
      }

      void markMonitorExited() noexcept
      {
        auto const lock = std::scoped_lock{_lifecycleMutex};
        _monitorExited = true;
        updateCompletionLocked();
      }

      void updateCompletionLocked() noexcept
      {
        if (_lifecycle == Lifecycle::Stopping && _listenerRetired && _registriesRetired && _threadRetired &&
            _monitorExited && _activeCallbackCount == 0U)
        {
          _lifecycle = Lifecycle::Stopped;
          _completionChanged.notify_all();
        }
      }

      void notifyShutdownStarted() const noexcept
      {
        if (_monitorStatePtr->monitorHooksPtr)
        {
          invokeHook(_monitorStatePtr->monitorHooksPtr->onShutdownStarted, "Core Audio shutdown-start observer");
        }
      }

      void notifyShutdownWait() const noexcept
      {
        if (_monitorStatePtr->monitorHooksPtr)
        {
          invokeHook(_monitorStatePtr->monitorHooksPtr->onShutdownWait, "Core Audio shutdown-wait observer");
        }
      }

      std::shared_ptr<detail::BackendDeviceRegistry> _deviceRegistryPtr;
      std::shared_ptr<detail::BackendGraphRegistry> _graphRegistryPtr;
      std::shared_ptr<CoreAudioMonitorState> _monitorStatePtr;
      std::byte _monitorScopeIdentity{};
      mutable std::mutex _lifecycleMutex;
      std::condition_variable _completionChanged;
      std::jthread _monitorThread;
      std::size_t _activeCallbackCount = 0U;
      Lifecycle _lifecycle = Lifecycle::Running;
      bool _listenerRetired = false;
      bool _registriesRetired = false;
      bool _threadRetired = false;
      bool _monitorExited = false;
    };
  } // namespace

  struct CoreAudioProvider::Impl final
  {
    std::shared_ptr<CoreAudioProviderControl> controlPtr;

    explicit Impl(std::shared_ptr<detail::CoreAudioProviderMonitorHooks> monitorHooksPtr)
      : controlPtr{std::make_shared<CoreAudioProviderControl>(std::move(monitorHooksPtr))}
    {
      controlPtr->startMonitor();
    }

    ~Impl()
    {
      auto const retainedControlPtr = controlPtr;
      retainedControlPtr->shutdown();
    }

    Impl(Impl const&) = delete;
    Impl& operator=(Impl const&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;
  };

  CoreAudioProvider::CoreAudioProvider()
    : CoreAudioProvider{nullptr}
  {
  }

  CoreAudioProvider::CoreAudioProvider(std::shared_ptr<detail::CoreAudioProviderMonitorHooks> monitorHooksPtr)
    : _implPtr{std::make_unique<Impl>(std::move(monitorHooksPtr))}
  {
  }

  CoreAudioProvider::~CoreAudioProvider()
  {
    shutdown();
  }

  void CoreAudioProvider::shutdown() noexcept
  {
    auto const controlPtr = _implPtr->controlPtr;
    controlPtr->shutdown();
  }

  Subscription CoreAudioProvider::subscribeDevices(OnDevicesChangedCallback callback)
  {
    auto const controlPtr = _implPtr->controlPtr;
    return controlPtr->subscribeDevices(std::move(callback));
  }

  BackendProvider::Status CoreAudioProvider::status() const
  {
    auto const controlPtr = _implPtr->controlPtr;
    return {.descriptor = {.id = kBackendCoreAudio, .supportedProfiles = {{.id = kProfileShared}}},
            .devices = controlPtr->devices()};
  }

  std::unique_ptr<Backend> CoreAudioProvider::createBackend(Device const& device, ProfileId const& /*profile*/)
  {
    auto const controlPtr = _implPtr->controlPtr;
    return std::make_unique<CoreAudioBackend>(device, kProfileShared, controlPtr->graphRegistry());
  }

  Subscription CoreAudioProvider::subscribeGraph(std::string_view const routeAnchor, OnGraphChangedCallback callback)
  {
    auto const controlPtr = _implPtr->controlPtr;
    return controlPtr->subscribeGraph(routeAnchor, std::move(callback));
  }
} // namespace ao::audio::backend
