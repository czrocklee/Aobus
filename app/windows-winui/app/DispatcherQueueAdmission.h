// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>

namespace ao::winui::detail
{
  enum class DispatcherQueueAdmissionState : std::uint8_t
  {
    Running,
    Closing,
    Draining,
    Closed,
  };

  enum class DispatcherQueueWakeRejectionDisposition : std::uint8_t
  {
    Fatal,
    ExpectedDuringClosure,
  };

  constexpr bool isTaskAdmissionOpen(DispatcherQueueAdmissionState const state, bool const ownerThread) noexcept
  {
    return state == DispatcherQueueAdmissionState::Running || state == DispatcherQueueAdmissionState::Closing ||
           (state == DispatcherQueueAdmissionState::Draining && ownerThread);
  }

  constexpr DispatcherQueueWakeRejectionDisposition wakeRejectionDisposition(
    DispatcherQueueAdmissionState const state) noexcept
  {
    return state == DispatcherQueueAdmissionState::Running
             ? DispatcherQueueWakeRejectionDisposition::Fatal
             : DispatcherQueueWakeRejectionDisposition::ExpectedDuringClosure;
  }

  class DispatcherQueueAdmission final
  {
  public:
    class [[nodiscard]] AdmissionTicket final
    {
    public:
      AdmissionTicket(AdmissionTicket const&) = delete;
      AdmissionTicket& operator=(AdmissionTicket const&) = delete;
      AdmissionTicket(AdmissionTicket&& other) noexcept;
      AdmissionTicket& operator=(AdmissionTicket&& other) noexcept;
      ~AdmissionTicket();

      void reset() noexcept;

    private:
      friend class DispatcherQueueAdmission;

      AdmissionTicket(DispatcherQueueAdmission& admission, bool ownerThread) noexcept;

      DispatcherQueueAdmission* _admission;
      bool _ownerThread;
    };

    DispatcherQueueAdmission() = default;
    ~DispatcherQueueAdmission() = default;

    DispatcherQueueAdmission(DispatcherQueueAdmission const&) = delete;
    DispatcherQueueAdmission& operator=(DispatcherQueueAdmission const&) = delete;
    DispatcherQueueAdmission(DispatcherQueueAdmission&&) = delete;
    DispatcherQueueAdmission& operator=(DispatcherQueueAdmission&&) = delete;

    std::optional<AdmissionTicket> tryAcquire(bool ownerThread);

    // Closing accepts work while the runtime stops and joins its producers.
    bool beginClosing() noexcept;
    // Draining seals foreign admission, waits for entered submissions, and
    // leaves owner-thread continuations open for the synchronous final drain.
    bool beginDraining() noexcept;
    bool finishClosing() noexcept;
    // Destruction fallback for construction failures and already-quiesced
    // owners. Seals admission and waits for entered foreign submissions under
    // one lock; queued task execution remains the executor owner's policy.
    bool closeForDestruction() noexcept;

    DispatcherQueueAdmissionState state() const;

  private:
    void release(bool ownerThread) noexcept;

    mutable std::mutex _mutex;
    std::condition_variable _admissionsDrained;
    DispatcherQueueAdmissionState _state = DispatcherQueueAdmissionState::Running;
    std::size_t _activeAdmissions = 0;
    std::size_t _ownerAdmissions = 0;
  };
} // namespace ao::winui::detail
