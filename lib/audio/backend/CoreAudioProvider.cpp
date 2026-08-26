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
#include <ao/utility/ThreadName.h>

#include <CoreAudio/AudioHardware.h>
#include <CoreAudio/AudioHardwareBase.h>
#include <MacTypes.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <exception>
#include <memory>
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

        if (monitorHooksPtr && monitorHooksPtr->onMonitorStateDestroyed)
        {
          try
          {
            monitorHooksPtr->onMonitorStateDestroyed();
          }
          catch (...)
          {
            AO_FATAL_EXCEPTION(std::current_exception(), "Core Audio monitor destruction observer");
          }
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

          if (monitorHooksPtr && monitorHooksPtr->onRefreshComplete)
          {
            monitorHooksPtr->onRefreshComplete();
          }
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
  } // namespace

  struct CoreAudioProvider::Impl final
  {
    std::shared_ptr<detail::BackendDeviceRegistry> deviceRegistryPtr =
      std::make_shared<detail::BackendDeviceRegistry>();
    std::shared_ptr<detail::BackendGraphRegistry> graphRegistryPtr = std::make_shared<detail::BackendGraphRegistry>();
    std::shared_ptr<CoreAudioMonitorState> monitorStatePtr;
    std::jthread monitorThread;
    std::atomic<bool> shutdownStarted{false};

    explicit Impl(std::shared_ptr<detail::CoreAudioProviderMonitorHooks> monitorHooksPtr)
      : monitorStatePtr{std::make_shared<CoreAudioMonitorState>(deviceRegistryPtr, std::move(monitorHooksPtr))}
    {
      if (monitorStatePtr->monitorHooksPtr)
      {
        monitorStatePtr->monitorHooksPtr->requestRefresh = [weakStatePtr = std::weak_ptr{monitorStatePtr}]
        {
          if (auto const statePtr = weakStatePtr.lock(); statePtr)
          {
            statePtr->changeSignal.release();
          }
        };
      }

      if (monitorStatePtr->installNativeListeners())
      {
        monitorStatePtr->deviceRegistryPtr->publish(monitorStatePtr->enumerateDevices());
        monitorThread =
          std::jthread{[statePtr = monitorStatePtr]
                       {
                         try
                         {
                           setCurrentThreadName("CoreAudioMonitor");
                           statePtr->monitorLoop();

                           if (statePtr->monitorHooksPtr && statePtr->monitorHooksPtr->onMonitorExit)
                           {
                             statePtr->monitorHooksPtr->onMonitorExit();
                           }
                         }
                         catch (...)
                         {
                           AO_FATAL_EXCEPTION(std::current_exception(), "Core Audio device-monitor thread");
                         }
                       }};
      }
    }

    ~Impl() { shutdown(); }

    Impl(Impl const&) = delete;
    Impl& operator=(Impl const&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    void shutdown() noexcept
    {
      if (shutdownStarted.exchange(true, std::memory_order_acq_rel))
      {
        return;
      }

      auto const deviceRegistryPtr = this->deviceRegistryPtr;
      auto const graphRegistryPtr = this->graphRegistryPtr;
      monitorStatePtr->requestShutdown();

      if (monitorThread.joinable())
      {
        if (monitorThread.get_id() == std::this_thread::get_id())
        {
          monitorThread.detach();
        }
        else
        {
          monitorThread.join();
        }
      }

      deviceRegistryPtr->shutdown();
      graphRegistryPtr->shutdown();
    }
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
    _implPtr->shutdown();
  }

  Subscription CoreAudioProvider::subscribeDevices(OnDevicesChangedCallback callback)
  {
    return _implPtr->deviceRegistryPtr->subscribe(std::move(callback));
  }

  BackendProvider::Status CoreAudioProvider::status() const
  {
    return {.descriptor = {.id = kBackendCoreAudio, .supportedProfiles = {{.id = kProfileShared}}},
            .devices = _implPtr->deviceRegistryPtr->snapshot()};
  }

  std::unique_ptr<Backend> CoreAudioProvider::createBackend(Device const& device, ProfileId const& /*profile*/)
  {
    return std::make_unique<CoreAudioBackend>(device, kProfileShared, _implPtr->graphRegistryPtr);
  }

  Subscription CoreAudioProvider::subscribeGraph(std::string_view const routeAnchor, OnGraphChangedCallback callback)
  {
    return _implPtr->graphRegistryPtr->subscribe(routeAnchor, std::move(callback));
  }
} // namespace ao::audio::backend
