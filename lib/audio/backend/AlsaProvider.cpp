// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnull-dereference"

#include "backend/AlsaProvider.h"

#include "backend/AlsaExclusiveBackend.h"
#include "backend/detail/AlsaDeviceDiscovery.h"
#include "backend/detail/AlsaGraphRegistry.h"
#include "backend/detail/AlsaProviderMonitorHooks.h"
#include "backend/detail/BackendDeviceRegistry.h"
#include "backend/detail/BackendObserverDispatcher.h"
#include <ao/Contract.h>
#include <ao/audio/Backend.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/BackendProvider.h>
#include <ao/audio/Device.h>
#include <ao/audio/flow/Graph.h>
#include <ao/utility/CallbackStackScope.h>
#include <ao/utility/Raii.h>
#include <ao/utility/ScopedRegistration.h>
#include <ao/utility/ThreadName.h>

#include <poll.h>

extern "C"
{
#include <libudev.h>
}

#pragma GCC diagnostic pop

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
#include <semaphore>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

namespace ao::audio::backend
{
  namespace
  {
    constexpr auto kUdevPollTimeout = std::chrono::milliseconds{500};

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

    struct AlsaMonitorState final
    {
      std::shared_ptr<detail::BackendDeviceRegistry> deviceRegistryPtr;
      std::shared_ptr<detail::AlsaProviderMonitorHooks> monitorHooksPtr;
      std::counting_semaphore<> refreshSignal{0};
      std::atomic<bool> shutdownRequested{false};

      AlsaMonitorState(std::shared_ptr<detail::BackendDeviceRegistry> registryPtr,
                       std::shared_ptr<detail::AlsaProviderMonitorHooks> hooksPtr)
        : deviceRegistryPtr{std::move(registryPtr)}, monitorHooksPtr{std::move(hooksPtr)}
      {
        deviceRegistryPtr->publish(enumerateDevices());
      }

      ~AlsaMonitorState()
      {
        requestShutdown();

        if (monitorHooksPtr)
        {
          invokeHook(monitorHooksPtr->onMonitorStateDestroyed, "ALSA monitor destruction observer");
        }
      }

      AlsaMonitorState(AlsaMonitorState const&) = delete;
      AlsaMonitorState& operator=(AlsaMonitorState const&) = delete;
      AlsaMonitorState(AlsaMonitorState&&) = delete;
      AlsaMonitorState& operator=(AlsaMonitorState&&) = delete;

      std::vector<Device> enumerateDevices() const
      {
        if (monitorHooksPtr && monitorHooksPtr->enumerateDevices)
        {
          return monitorHooksPtr->enumerateDevices();
        }

        return detail::enumerateAlsaPlaybackDevices();
      }

      bool isStopping(std::stop_token const& stopToken) const noexcept
      {
        return stopToken.stop_requested() || shutdownRequested.load(std::memory_order_acquire);
      }

      void refresh(std::stop_token const& stopToken)
      {
        auto devices = enumerateDevices();

        if (isStopping(stopToken))
        {
          return;
        }

        deviceRegistryPtr->publish(std::move(devices));

        if (!isStopping(stopToken) && monitorHooksPtr)
        {
          invokeHook(monitorHooksPtr->onRefreshComplete, "ALSA monitor refresh observer");
        }
      }

      void injectedMonitorLoop(std::stop_token const& stopToken)
      {
        while (!isStopping(stopToken))
        {
          refreshSignal.acquire();

          if (isStopping(stopToken))
          {
            return;
          }

          refresh(stopToken);
        }
      }

      void nativeMonitorLoop(std::stop_token const& stopToken)
      {
        auto udevPtr = utility::makeUniquePtr<::udev_unref>(::udev_new());

        if (!udevPtr)
        {
          return;
        }

        auto monitorPtr =
          utility::makeUniquePtr<::udev_monitor_unref>(::udev_monitor_new_from_netlink(udevPtr.get(), "udev"));

        if (!monitorPtr)
        {
          return;
        }

        ::udev_monitor_filter_add_match_subsystem_devtype(monitorPtr.get(), "sound", nullptr);
        ::udev_monitor_enable_receiving(monitorPtr.get());
        auto const fd = ::udev_monitor_get_fd(monitorPtr.get());

        while (!isStopping(stopToken))
        {
          auto fds = std::array<struct pollfd, 1>{};
          fds[0].fd = fd;
          fds[0].events = POLLIN;

          if (::poll(fds.data(), static_cast<nfds_t>(fds.size()), static_cast<std::int32_t>(kUdevPollTimeout.count())) >
                0 &&
              (fds[0].revents & POLLIN) != 0)
          {
            auto devPtr = utility::makeUniquePtr<::udev_device_unref>(::udev_monitor_receive_device(monitorPtr.get()));

            if (devPtr)
            {
              refresh(stopToken);
            }
          }
        }
      }

      void monitorLoop(std::stop_token const& stopToken)
      {
        if (monitorHooksPtr && monitorHooksPtr->enumerateDevices)
        {
          injectedMonitorLoop(stopToken);
          return;
        }

        nativeMonitorLoop(stopToken);
      }

      void requestRefresh() noexcept
      {
        if (!shutdownRequested.load(std::memory_order_acquire))
        {
          refreshSignal.release();
        }
      }

      void requestShutdown() noexcept
      {
        if (!shutdownRequested.exchange(true, std::memory_order_acq_rel))
        {
          refreshSignal.release();
        }
      }
    };

    class AlsaProviderControl final : public std::enable_shared_from_this<AlsaProviderControl>
    {
    public:
      explicit AlsaProviderControl(std::shared_ptr<detail::AlsaProviderMonitorHooks> monitorHooksPtr)
        : _dispatcherPtr{std::make_shared<detail::BackendObserverDispatcher>()}
        , _deviceRegistryPtr{std::make_shared<detail::BackendDeviceRegistry>(_dispatcherPtr)}
        , _graphRegistryPtr{std::make_shared<detail::AlsaGraphRegistry>(_dispatcherPtr)}
        , _monitorStatePtr{std::make_shared<AlsaMonitorState>(_deviceRegistryPtr, std::move(monitorHooksPtr))}
      {
      }

      ~AlsaProviderControl() = default;

      AlsaProviderControl(AlsaProviderControl const&) = delete;
      AlsaProviderControl& operator=(AlsaProviderControl const&) = delete;
      AlsaProviderControl(AlsaProviderControl&&) = delete;
      AlsaProviderControl& operator=(AlsaProviderControl&&) = delete;

      void startMonitor()
      {
        if (auto const weakStatePtr = std::weak_ptr{_monitorStatePtr}; _monitorStatePtr->monitorHooksPtr)
        {
          _monitorStatePtr->monitorHooksPtr->requestRefresh = [weakStatePtr]
          {
            if (auto const statePtr = weakStatePtr.lock(); statePtr)
            {
              statePtr->requestRefresh();
            }
          };
        }

        _monitorThread =
          std::jthread{[controlPtr = shared_from_this()](std::stop_token const& stopToken)
                       {
                         auto& state = *controlPtr->_monitorStatePtr;
                         auto const monitorScope = utility::CallbackStackScope{&state};

                         try
                         {
                           setCurrentThreadName("AlsaDeviceMonitor");
                           state.monitorLoop(stopToken);

                           if (state.monitorHooksPtr)
                           {
                             invokeHook(state.monitorHooksPtr->onMonitorExit, "ALSA monitor-exit observer");
                           }
                         }
                         catch (...)
                         {
                           AO_FATAL_EXCEPTION(std::current_exception(), "ALSA device-monitor thread");
                         }

                         controlPtr->markMonitorExited();
                       }};
      }

      void shutdown() noexcept
      {
        bool const returnWithoutWaiting = _dispatcherPtr->isDispatchingOnThisThread() ||
                                          utility::CallbackStackScope::containsIdentity(_monitorStatePtr.get());

        {
          auto lock = std::unique_lock{_mutex};

          if (_lifecycle == Lifecycle::Stopped)
          {
            return;
          }

          if (_lifecycle == Lifecycle::Stopping)
          {
            // Retiring a registration can destroy a subscriber that calls shutdown again.
            if (returnWithoutWaiting || _retiringThread == std::this_thread::get_id())
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
          _retiringThread = std::this_thread::get_id();
        }

        notifyShutdownStarted();

        _monitorStatePtr->requestShutdown();
        _monitorThread.request_stop();

        // Retire graph publishers before a backend can observe provider storage
        // disappearing, then close device callbacks before joining their source.
        _graphRegistryPtr->shutdown(!returnWithoutWaiting);
        _deviceRegistryPtr->shutdown();

        if (_monitorThread.joinable())
        {
          if (returnWithoutWaiting)
          {
            // The worker retains control through its exit notification. Let
            // the callback unwind instead of joining a worker that needs it.
            _monitorThread.detach();
          }
          else
          {
            _monitorThread.join();
          }
        }

        {
          auto lock = std::unique_lock{_mutex};
          _retiringThread = {};
          _sourcesRetired = true;
          updateCompletionLocked();

          if (returnWithoutWaiting)
          {
            return;
          }

          _completionChanged.wait(lock, [this] { return _lifecycle == Lifecycle::Stopped; });
        }
      }

      utility::ScopedRegistration subscribeDevices(BackendProvider::OnDevicesChangedCallback callback)
      {
        if (!callback || !acceptsSubscriptions())
        {
          return {};
        }

        auto const weakControlPtr = weak_from_this();
        auto sub = _deviceRegistryPtr->subscribe(
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

      utility::ScopedRegistration subscribeGraph(std::string_view const routeAnchor,
                                                 BackendProvider::OnGraphChangedCallback callback)
      {
        if (!callback || !acceptsSubscriptions())
        {
          return {};
        }

        auto const weakControlPtr = weak_from_this();
        auto sub = _graphRegistryPtr->subscribeWithDelivery(
          routeAnchor,
          [weakControlPtr, callback = std::move(callback)](flow::Graph const& graph, bool const isTerminal)
          {
            if (auto const controlPtr = weakControlPtr.lock(); controlPtr)
            {
              controlPtr->invokeGraphCallback(callback, graph, isTerminal);
            }
          });

        if (!acceptsSubscriptions())
        {
          sub.reset();
          return {};
        }

        return sub;
      }

      std::vector<Device> devices() const
      {
        return acceptsSubscriptions() ? _deviceRegistryPtr->snapshot() : std::vector<Device>{};
      }

      detail::AlsaGraphPublisher graphPublisher() const { return _graphRegistryPtr->publisher(); }

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
        explicit CallbackScope(AlsaProviderControl& control, bool const isTerminalGraph = false)
          : _control{control}
        {
          auto const lock = std::scoped_lock{_control._mutex};

          if (_control._lifecycle == Lifecycle::Running ||
              (_control._lifecycle == Lifecycle::Stopping && isTerminalGraph))
          {
            ++_control._activeCallbackCount;
            _admitted = true;
          }
        }

        ~CallbackScope()
        {
          if (!_admitted)
          {
            return;
          }

          auto const lock = std::scoped_lock{_control._mutex};
          --_control._activeCallbackCount;
          _control.updateCompletionLocked();
        }

        CallbackScope(CallbackScope const&) = delete;
        CallbackScope& operator=(CallbackScope const&) = delete;
        CallbackScope(CallbackScope&&) = delete;
        CallbackScope& operator=(CallbackScope&&) = delete;

        bool isAdmitted() const noexcept { return _admitted; }

      private:
        AlsaProviderControl& _control;
        bool _admitted = false;
      };

      bool acceptsSubscriptions() const
      {
        auto const lock = std::scoped_lock{_mutex};
        return _lifecycle == Lifecycle::Running;
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

      void invokeGraphCallback(BackendProvider::OnGraphChangedCallback const& callback,
                               flow::Graph const& graph,
                               bool const isTerminal)
      {
        auto const scope = CallbackScope{*this, isTerminal};

        if (scope.isAdmitted())
        {
          callback(graph);
        }
      }

      void markMonitorExited() noexcept
      {
        auto const lock = std::scoped_lock{_mutex};
        _monitorExited = true;
        updateCompletionLocked();
      }

      void updateCompletionLocked() noexcept
      {
        if (_lifecycle == Lifecycle::Stopping && _sourcesRetired && _monitorExited && _activeCallbackCount == 0U)
        {
          _lifecycle = Lifecycle::Stopped;
          _completionChanged.notify_all();
        }
      }

      void notifyShutdownStarted() const noexcept
      {
        if (_monitorStatePtr->monitorHooksPtr)
        {
          invokeHook(_monitorStatePtr->monitorHooksPtr->onShutdownStarted, "ALSA shutdown-start observer");
        }
      }

      void notifyShutdownWait() const noexcept
      {
        if (_monitorStatePtr->monitorHooksPtr)
        {
          invokeHook(_monitorStatePtr->monitorHooksPtr->onShutdownWait, "ALSA shutdown-wait observer");
        }
      }

      std::shared_ptr<detail::BackendObserverDispatcher> _dispatcherPtr;
      std::shared_ptr<detail::BackendDeviceRegistry> _deviceRegistryPtr;
      std::shared_ptr<detail::AlsaGraphRegistry> _graphRegistryPtr;
      std::shared_ptr<AlsaMonitorState> _monitorStatePtr;
      mutable std::mutex _mutex;
      std::condition_variable _completionChanged;
      std::jthread _monitorThread;
      std::size_t _activeCallbackCount = 0U;
      Lifecycle _lifecycle = Lifecycle::Running;
      std::thread::id _retiringThread{};
      bool _sourcesRetired = false;
      bool _monitorExited = false;
    };
  } // namespace

  struct AlsaProvider::Impl final
  {
    std::shared_ptr<AlsaProviderControl> controlPtr;

    explicit Impl(std::shared_ptr<detail::AlsaProviderMonitorHooks> monitorHooksPtr)
      : controlPtr{std::make_shared<AlsaProviderControl>(std::move(monitorHooksPtr))}
    {
      controlPtr->startMonitor();
    }

    ~Impl() = default;

    Impl(Impl const&) = delete;
    Impl& operator=(Impl const&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;
  };

  AlsaProvider::AlsaProvider()
    : AlsaProvider{nullptr}
  {
  }

  AlsaProvider::AlsaProvider(std::shared_ptr<detail::AlsaProviderMonitorHooks> monitorHooksPtr)
    : _implPtr{std::make_unique<Impl>(std::move(monitorHooksPtr))}
  {
  }

  AlsaProvider::~AlsaProvider()
  {
    shutdown();
  }

  void AlsaProvider::shutdown() noexcept
  {
    auto const controlPtr = _implPtr->controlPtr;
    controlPtr->shutdown();
  }

  utility::ScopedRegistration AlsaProvider::subscribeDevices(OnDevicesChangedCallback callback)
  {
    auto const controlPtr = _implPtr->controlPtr;
    return controlPtr->subscribeDevices(std::move(callback));
  }

  BackendProvider::Status AlsaProvider::status() const
  {
    auto const controlPtr = _implPtr->controlPtr;
    return {.descriptor = {.id = kBackendAlsa, .supportedProfiles = {{.id = kProfileExclusive}}},
            .devices = controlPtr->devices()};
  }

  std::unique_ptr<Backend> AlsaProvider::createBackend(Device const& device, ProfileId const& /*profile*/)
  {
    auto const controlPtr = _implPtr->controlPtr;
    return std::make_unique<AlsaExclusiveBackend>(device, kProfileExclusive, controlPtr->graphPublisher());
  }

  utility::ScopedRegistration AlsaProvider::subscribeGraph(std::string_view const routeAnchor,
                                                           OnGraphChangedCallback callback)
  {
    auto const controlPtr = _implPtr->controlPtr;
    return controlPtr->subscribeGraph(routeAnchor, std::move(callback));
  }
} // namespace ao::audio::backend
