// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <memory>
#include <utility>

namespace ao::winui
{
  namespace detail
  {
    struct CallbackAdmissionState final
    {};
  } // namespace detail

  /**
   * Dispatcher-confined admission for callbacks that borrow a raw owner.
   *
   * A Token only reports whether its generation still admits callbacks. It does
   * not retain or protect the owner's memory. The owner must retire the gate
   * before cancelling work or draining the dispatcher, and must remain alive
   * until every admitted callback has settled by that separate protocol.
   */
  class CallbackAdmissionGate final
  {
  public:
    class Token final
    {
    public:
      Token() = default;

      bool admits() const noexcept { return !_statePtr.expired(); }

    private:
      friend class CallbackAdmissionGate;
      explicit Token(std::weak_ptr<detail::CallbackAdmissionState> statePtr) noexcept
        : _statePtr{std::move(statePtr)}
      {
      }

      std::weak_ptr<detail::CallbackAdmissionState> _statePtr;
    };

    CallbackAdmissionGate()
      : _statePtr{std::make_shared<detail::CallbackAdmissionState>()}
    {
    }
    ~CallbackAdmissionGate() = default;

    CallbackAdmissionGate(CallbackAdmissionGate const&) = delete;
    CallbackAdmissionGate& operator=(CallbackAdmissionGate const&) = delete;
    CallbackAdmissionGate(CallbackAdmissionGate&&) = delete;
    CallbackAdmissionGate& operator=(CallbackAdmissionGate&&) = delete;

    Token token() const noexcept { return Token{_statePtr}; }
    bool isOpen() const noexcept { return _statePtr != nullptr; }

    void retire() noexcept { _statePtr.reset(); }

    /** Retires every old token and opens one fresh workflow generation. */
    void renew() { _statePtr = std::make_shared<detail::CallbackAdmissionState>(); }

  private:
    std::shared_ptr<detail::CallbackAdmissionState> _statePtr;
  };
} // namespace ao::winui
