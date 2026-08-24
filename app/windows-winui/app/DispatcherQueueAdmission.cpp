// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "DispatcherQueueAdmission.h"

#include <ao/Contract.h>

#include <mutex>
#include <optional>
#include <utility>

namespace ao::winui::detail
{
  DispatcherQueueAdmission::AdmissionTicket::AdmissionTicket(DispatcherQueueAdmission& admission,
                                                             bool const ownerThread) noexcept
    : _admission{&admission}, _ownerThread{ownerThread}
  {
  }

  DispatcherQueueAdmission::AdmissionTicket::AdmissionTicket(AdmissionTicket&& other) noexcept
    : _admission{std::exchange(other._admission, nullptr)}, _ownerThread{other._ownerThread}
  {
  }

  DispatcherQueueAdmission::AdmissionTicket& DispatcherQueueAdmission::AdmissionTicket::operator=(
    AdmissionTicket&& other) noexcept
  {
    if (this != &other)
    {
      reset();
      _admission = std::exchange(other._admission, nullptr);
      _ownerThread = other._ownerThread;
    }

    return *this;
  }

  DispatcherQueueAdmission::AdmissionTicket::~AdmissionTicket()
  {
    reset();
  }

  void DispatcherQueueAdmission::AdmissionTicket::reset() noexcept
  {
    if (auto* const admission = std::exchange(_admission, nullptr); admission != nullptr)
    {
      admission->release(_ownerThread);
    }
  }

  std::optional<DispatcherQueueAdmission::AdmissionTicket> DispatcherQueueAdmission::tryAcquire(bool const ownerThread)
  {
    auto const lock = std::scoped_lock{_mutex};

    if (!isTaskAdmissionOpen(_state, ownerThread))
    {
      return std::nullopt;
    }

    ++_activeAdmissions;

    if (ownerThread)
    {
      ++_ownerAdmissions;
    }

    return std::optional<AdmissionTicket>{AdmissionTicket{*this, ownerThread}};
  }

  bool DispatcherQueueAdmission::beginClosing() noexcept
  {
    auto const lock = std::scoped_lock{_mutex};

    if (_state == DispatcherQueueAdmissionState::Running)
    {
      _state = DispatcherQueueAdmissionState::Closing;
    }

    return _state == DispatcherQueueAdmissionState::Closing;
  }

  bool DispatcherQueueAdmission::beginDraining() noexcept
  {
    auto lock = std::unique_lock{_mutex};

    if (_state != DispatcherQueueAdmissionState::Closing || _ownerAdmissions != 0)
    {
      return false;
    }

    _state = DispatcherQueueAdmissionState::Draining;
    _admissionsDrained.wait(lock, [this] { return _activeAdmissions == 0; });
    return true;
  }

  bool DispatcherQueueAdmission::finishClosing() noexcept
  {
    auto const lock = std::scoped_lock{_mutex};

    if (_state != DispatcherQueueAdmissionState::Draining || _activeAdmissions != 0)
    {
      return false;
    }

    _state = DispatcherQueueAdmissionState::Closed;
    return true;
  }

  bool DispatcherQueueAdmission::closeForDestruction() noexcept
  {
    auto lock = std::unique_lock{_mutex};

    if (_state == DispatcherQueueAdmissionState::Closed)
    {
      return true;
    }

    if (_ownerAdmissions != 0)
    {
      return false;
    }

    _state = DispatcherQueueAdmissionState::Draining;
    _admissionsDrained.wait(lock, [this] { return _activeAdmissions == 0; });
    _state = DispatcherQueueAdmissionState::Closed;
    return true;
  }

  DispatcherQueueAdmissionState DispatcherQueueAdmission::state() const
  {
    auto const lock = std::scoped_lock{_mutex};
    return _state;
  }

  void DispatcherQueueAdmission::release(bool const ownerThread) noexcept
  {
    auto const lock = std::scoped_lock{_mutex};
    AO_INVARIANT(_activeAdmissions != 0);
    --_activeAdmissions;

    if (ownerThread)
    {
      AO_INVARIANT(_ownerAdmissions != 0);
      --_ownerAdmissions;
    }

    if (_activeAdmissions == 0)
    {
      _admissionsDrained.notify_all();
    }
  }
} // namespace ao::winui::detail
