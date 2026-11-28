// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <condition_variable>
#include <cstdint>
#include <latch>
#include <mutex>
#include <stop_token>
#include <thread>
#include <utility>

namespace ao::audio::backend::detail
{
  // Device and graph observers of one provider run serially on the publishing
  // thread. This object owns no observers, tickets, registry data or workers.
  // Its mutex protects only selection of the current publisher.
  class BackendObserverDispatcher final
  {
  public:
    template<typename Deliver>
    void dispatch(Deliver&& deliver, std::stop_token stop = {})
    {
      switch (claimPublisher(stop))
      {
        case Admission::Cancelled: return;
        case Admission::CurrentPublisher:
          // Synchronous initial and final snapshots may run on this stack.
          // Graph publication itself defers reentrant work to its outer drain.
          std::forward<Deliver>(deliver)();
          return;
        case Admission::NewPublisher:
        {
          auto const turn = PublisherTurn{*this};
          std::forward<Deliver>(deliver)();
          return;
        }
      }
    }

    bool isDispatchingOnThisThread() const noexcept;

    // Test handoff: keep the current publisher busy and the contender's token
    // unstopped until this returns. Under those conditions, the contender has
    // released the dispatcher mutex inside the cancellable wait.
    void observeNextPublisherWait(std::latch& armed);

  private:
    enum class Admission : std::uint8_t
    {
      Cancelled,
      CurrentPublisher,
      NewPublisher,
    };

    struct PublisherTurn final
    {
      explicit PublisherTurn(BackendObserverDispatcher& owner)
        : dispatcher{owner}
      {
      }

      ~PublisherTurn() { dispatcher.finishPublication(); }
      PublisherTurn(PublisherTurn const&) = delete;
      PublisherTurn& operator=(PublisherTurn const&) = delete;
      PublisherTurn(PublisherTurn&&) = delete;
      PublisherTurn& operator=(PublisherTurn&&) = delete;

      BackendObserverDispatcher& dispatcher;
    };

    Admission claimPublisher(std::stop_token stop);
    void finishPublication() noexcept;

    mutable std::mutex _mutex;
    std::condition_variable_any _publisherChanged;
    std::thread::id _publisher{};
    std::condition_variable* _waitObserver = nullptr;
  };
} // namespace ao::audio::backend::detail
