// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/utility/TaggedInteger.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace ao::app
{
  using ViewId = ao::utility::TaggedInteger<std::uint64_t, struct ViewIdTag>;
  using NotificationId = ao::utility::TaggedInteger<std::uint64_t, struct NotificationIdTag>;

  struct Range final
  {
    std::size_t start = 0;
    std::size_t count = 0;
  };

  class Subscription final
  {
  public:
    Subscription() = default;

    explicit Subscription(std::move_only_function<void()> unsubscribe)
      : _unsubscribe{std::move(unsubscribe)}
    {
    }

    Subscription(Subscription const&) = delete;
    Subscription& operator=(Subscription const&) = delete;

    Subscription(Subscription&&) noexcept = default;
    Subscription& operator=(Subscription&&) noexcept = default;

    ~Subscription()
    {
      if (_unsubscribe)
      {
        _unsubscribe();
      }
    }

    void reset()
    {
      if (_unsubscribe)
      {
        _unsubscribe();
        _unsubscribe = {};
      }
    }

    explicit operator bool() const noexcept { return static_cast<bool>(_unsubscribe); }

  private:
    std::move_only_function<void()> _unsubscribe;
  };

  class IControlExecutor
  {
  public:
    virtual ~IControlExecutor() = default;

    virtual bool isCurrent() const noexcept = 0;
    virtual void dispatch(std::move_only_function<void()> task) = 0;
  };

  enum class FaultDomain : std::uint8_t
  {
    Playback,
    Output,
    Library,
    Import,
    Query,
    View,
    Generic,
  };

  struct FaultSnapshot final
  {
    FaultDomain domain = FaultDomain::Generic;
    ao::Error error{};
    bool transient = false;
  };

  enum class StoreDeliveryMode : std::uint8_t
  {
    ReplayCurrent,
    FutureOnly,
  };

  template<class State>
  class IReadOnlyStore
  {
  public:
    virtual ~IReadOnlyStore() = default;

    // NOLINTNEXTLINE(portability-template-virtual-member-function)
    virtual State snapshot() const = 0;
    // NOLINTNEXTLINE(portability-template-virtual-member-function)
    virtual Subscription subscribe(std::move_only_function<void(State const&)> handler,
                                   StoreDeliveryMode mode = StoreDeliveryMode::ReplayCurrent) = 0;
  };
}
