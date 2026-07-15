// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace ao::gtk
{
  /**
   * Bounds void callbacks to an owner's lifetime on one GLib main context.
   *
   * The owner, close(), and guarded callbacks must remain confined to the same
   * main context. The optional close callback runs once after invalidation,
   * must not throw, and may access only resources that outlive this scope.
   */
  class [[nodiscard]] MainContextCallbackScope final
  {
  public:
    explicit MainContextCallbackScope(std::function<void()> closeCallback = {})
      : _statePtr{std::make_shared<State>()}, _closeCallback{std::move(closeCallback)}
    {
    }

    ~MainContextCallbackScope() { close(); }

    MainContextCallbackScope(MainContextCallbackScope const&) = delete;
    MainContextCallbackScope& operator=(MainContextCallbackScope const&) = delete;
    MainContextCallbackScope(MainContextCallbackScope&&) = delete;
    MainContextCallbackScope& operator=(MainContextCallbackScope&&) = delete;

    template<typename Callback>
    auto guard(Callback callback) const
    {
      auto const weakStatePtr = std::weak_ptr<State>{_statePtr};
      // Generic callback arguments can be values or managed pointers.
      // NOLINTNEXTLINE(aobus-readability-pointer-naming-convention)
      return [weakStatePtr, callback = std::move(callback)](auto&&... arguments) mutable
      {
        static_assert(std::is_void_v<std::invoke_result_t<Callback&, decltype(arguments)...>>,
                      "MainContextCallbackScope only guards void callbacks");

        if (auto const statePtr = weakStatePtr.lock(); !statePtr)
        {
          return;
        }

        std::invoke(callback, std::forward<decltype(arguments)>(arguments)...);
      };
    }

    void close()
    {
      if (!_statePtr)
      {
        return;
      }

      _statePtr.reset();

      if (auto closeCallback = std::move(_closeCallback); closeCallback)
      {
        closeCallback();
      }
    }

  private:
    struct State final
    {};

    std::shared_ptr<State> _statePtr;
    std::function<void()> _closeCallback;
  };
} // namespace ao::gtk
