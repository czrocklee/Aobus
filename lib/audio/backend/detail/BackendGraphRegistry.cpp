// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "backend/detail/BackendGraphRegistry.h"

#include "backend/detail/BackendObserverDispatcher.h"
#include "backend/detail/BackendObserverSlot.h"
#include <ao/Contract.h>
#include <ao/audio/flow/Graph.h>
#include <ao/utility/ScopedRegistration.h>

#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ao::audio::backend::detail
{
  namespace
  {
    using Slot = BackendObserverSlot<BackendGraphRegistry::DeliveryCallback>;

    void notifySlot(Slot& slot, flow::Graph const& graph, bool const isTerminal, char const* const context) noexcept
    {
      try
      {
        slot.invoke([&](BackendGraphRegistry::DeliveryCallback const& callback) { callback(graph, isTerminal); });
      }
      catch (...)
      {
        AO_FATAL_EXCEPTION(std::current_exception(), context);
      }
    }
  } // namespace

  struct BackendGraphRegistry::State final
  {
    struct Subscriber final
    {
      std::string routeAnchor;
      std::shared_ptr<Slot> slotPtr;
    };

    struct Delivery final
    {
      std::string routeAnchor;
      // Initial subscription snapshots precede later publications and have no route revision.
      std::uint64_t revision = 0;
      flow::Graph graph{};
      std::vector<std::weak_ptr<Slot>> recipients{};
      char const* context = "audio backend graph observer";
    };

    explicit State(std::shared_ptr<BackendObserverDispatcher> sharedDispatcherPtr)
      : dispatcherPtr{std::move(sharedDispatcherPtr)}
    {
      AO_INVARIANT(dispatcherPtr);
    }

    static PendingDelivery makeDelivery(std::shared_ptr<State> const& statePtr, Delivery delivery)
    {
      if (delivery.recipients.empty())
      {
        return {};
      }

      return PendingDelivery{
        [weakStatePtr = std::weak_ptr{statePtr}, delivery = std::move(delivery)](std::stop_token stop) mutable
        {
          if (auto const statePtr = weakStatePtr.lock(); statePtr)
          {
            statePtr->dispatcherPtr->dispatch([&] { statePtr->drain(std::move(delivery)); }, std::move(stop));
          }
        }};
    }

    void snapshotRecipients(Delivery& delivery) const
    {
      for (auto const& subscriber : subscribers)
      {
        if (subscriber.routeAnchor == delivery.routeAnchor)
        {
          delivery.recipients.emplace_back(subscriber.slotPtr);
        }
      }
    }

    bool isCurrent(Delivery const& delivery) const
    {
      return !closed && (delivery.revision == 0 || revisions.at(delivery.routeAnchor) == delivery.revision);
    }

    // Called on a dispatcher turn. Competing publishers retain their tickets
    // on their own threads; only reentry into this registry appends to the drain.
    void drain(Delivery delivery)
    {
      publications.invoke(
        [&]
        {
          pending.push_back(std::move(delivery));

          if (draining)
          {
            return;
          }

          draining = true;

          while (!pending.empty())
          {
            auto next = std::move(pending.front());
            pending.pop_front();

            for (auto const& weakSlotPtr : next.recipients)
            {
              {
                auto const lock = std::scoped_lock{mutex};

                if (!isCurrent(next))
                {
                  break;
                }
              }

              if (auto const slotPtr = weakSlotPtr.lock(); slotPtr)
              {
                notifySlot(*slotPtr, next.graph, false, next.context);
              }
            }
          }

          draining = false;
        });
    }

    void deliverFinals(std::list<Subscriber>& retired)
    {
      for (;;)
      {
        {
          auto const lock = std::scoped_lock{mutex};

          if (subscribers.empty())
          {
            return;
          }

          // Claim only the next notification. A nested shutdown must drain or
          // revoke the remaining registrations before returning to this one.
          retired.splice(retired.end(), subscribers, subscribers.begin());
        }

        // Reset still reaches this slot directly and waits on its call lease.
        notifySlot(*retired.back().slotPtr, {}, true, "audio backend graph shutdown observer");
      }
    }

    std::shared_ptr<BackendObserverDispatcher> dispatcherPtr;
    BackendObserverCalls publications{};
    // Confined to a dispatcher turn. Tickets contain only weak observer references.
    std::deque<Delivery> pending{};
    bool draining = false;
    std::mutex mutex{};
    std::unordered_map<std::string, flow::Graph> graphs{};
    std::unordered_map<std::string, std::uint64_t> revisions{};
    std::list<Subscriber> subscribers{};
    std::uint64_t nextRevision = 1;
    bool closed = false;
  };

  BackendGraphRegistry::PendingDelivery::~PendingDelivery() = default;

  BackendGraphRegistry::PendingDelivery::PendingDelivery(std::function<void(std::stop_token)> callback) noexcept
    : _callback{std::move(callback)}
  {
  }

  BackendGraphRegistry::PendingDelivery::PendingDelivery(PendingDelivery&& other) noexcept
    : _callback{std::exchange(other._callback, {})}
  {
  }

  BackendGraphRegistry::PendingDelivery& BackendGraphRegistry::PendingDelivery::operator=(
    PendingDelivery&& other) noexcept
  {
    if (this != &other)
    {
      _callback = std::exchange(other._callback, {});
    }

    return *this;
  }

  void BackendGraphRegistry::PendingDelivery::deliver(std::stop_token stop) &&
  {
    if (auto callback = std::exchange(_callback, {}); callback)
    {
      callback(std::move(stop));
    }
  }

  BackendGraphRegistry::BackendGraphRegistry(std::shared_ptr<BackendObserverDispatcher> dispatcherPtr)
    : _statePtr{std::make_shared<State>(std::move(dispatcherPtr))}
  {
  }

  BackendGraphRegistry::~BackendGraphRegistry()
  {
    shutdown();
  }

  utility::ScopedRegistration BackendGraphRegistry::subscribe(std::string_view const routeAnchor,
                                                              Callback callback,
                                                              flow::Graph initialGraph)
  {
    if (!callback)
    {
      return {};
    }

    return subscribeWithDelivery(
      routeAnchor,
      [callback = std::move(callback)](flow::Graph const& graph, bool) { callback(graph); },
      std::move(initialGraph));
  }

  utility::ScopedRegistration BackendGraphRegistry::subscribeWithDelivery(std::string_view const routeAnchor,
                                                                          DeliveryCallback callback,
                                                                          flow::Graph initialGraph)
  {
    if (!callback)
    {
      return {};
    }

    auto const statePtr = _statePtr;
    {
      auto const lock = std::scoped_lock{statePtr->mutex};

      if (statePtr->closed)
      {
        return {};
      }
    }

    auto const slotPtr = std::make_shared<Slot>(std::move(callback));
    auto const anchor = std::string{routeAnchor};
    statePtr->dispatcherPtr->dispatch(
      [&]
      {
        auto delivery = State::Delivery{
          .routeAnchor = anchor, .recipients = {slotPtr}, .context = "audio backend initial graph observer"};
        {
          auto const lock = std::scoped_lock{statePtr->mutex};

          if (statePtr->closed)
          {
            return;
          }

          statePtr->subscribers.push_back({.routeAnchor = anchor, .slotPtr = slotPtr});
          auto const it = statePtr->graphs.find(anchor);
          delivery.graph = it != statePtr->graphs.end() ? it->second : std::move(initialGraph);
        }

        statePtr->drain(std::move(delivery));
      });

    bool closed = false;
    {
      auto const lock = std::scoped_lock{statePtr->mutex};
      closed = statePtr->closed;
    }

    if (closed)
    {
      slotPtr->retire();
      return {};
    }

    return utility::ScopedRegistration{
      [weakStatePtr = std::weak_ptr{statePtr}, weakSlotPtr = std::weak_ptr{slotPtr}]
      {
        auto const slotPtr = weakSlotPtr.lock();

        if (!slotPtr)
        {
          return;
        }

        slotPtr->retire();

        if (auto const statePtr = weakStatePtr.lock(); statePtr)
        {
          auto const lock = std::scoped_lock{statePtr->mutex};
          std::erase_if(statePtr->subscribers, [&](auto const& subscriber) { return subscriber.slotPtr == slotPtr; });
        }
      }};
  }

  BackendGraphRegistry::PendingDelivery BackendGraphRegistry::preparePublish(std::string_view const routeAnchor,
                                                                             flow::Graph graph)
  {
    auto const statePtr = _statePtr;
    auto delivery = State::Delivery{.routeAnchor = std::string{routeAnchor}, .graph = std::move(graph)};

    {
      auto const lock = std::scoped_lock{statePtr->mutex};

      if (statePtr->closed)
      {
        return {};
      }

      if (auto const it = statePtr->graphs.find(delivery.routeAnchor);
          it != statePtr->graphs.end() && it->second == delivery.graph)
      {
        return {};
      }

      statePtr->graphs[delivery.routeAnchor] = delivery.graph;
      delivery.revision = statePtr->nextRevision++;
      statePtr->revisions[delivery.routeAnchor] = delivery.revision;
      statePtr->snapshotRecipients(delivery);
    }

    return State::makeDelivery(statePtr, std::move(delivery));
  }

  BackendGraphRegistry::PendingDelivery BackendGraphRegistry::prepareClear(std::string_view const routeAnchor)
  {
    auto const statePtr = _statePtr;
    auto delivery = State::Delivery{.routeAnchor = std::string{routeAnchor}};

    {
      auto const lock = std::scoped_lock{statePtr->mutex};

      if (statePtr->closed)
      {
        return {};
      }

      statePtr->graphs.erase(delivery.routeAnchor);
      delivery.revision = statePtr->nextRevision++;
      statePtr->revisions[delivery.routeAnchor] = delivery.revision;
      statePtr->snapshotRecipients(delivery);
    }

    return State::makeDelivery(statePtr, std::move(delivery));
  }

  void BackendGraphRegistry::publish(std::string_view const routeAnchor, flow::Graph graph)
  {
    auto delivery = preparePublish(routeAnchor, std::move(graph));
    std::move(delivery).deliver();
  }

  void BackendGraphRegistry::clear(std::string_view const routeAnchor)
  {
    auto delivery = prepareClear(routeAnchor);
    std::move(delivery).deliver();
  }

  void BackendGraphRegistry::shutdown(bool const deliverTerminal) noexcept
  {
    auto const statePtr = _statePtr;
    auto retired = std::list<State::Subscriber>{};
    bool needsFinals = false;
    {
      auto const lock = std::scoped_lock{statePtr->mutex};
      statePtr->closed = true;
      statePtr->graphs.clear();
      statePtr->revisions.clear();

      if (deliverTerminal)
      {
        needsFinals = !statePtr->subscribers.empty();
      }
      else
      {
        retired.swap(statePtr->subscribers);
      }
    }

    for (auto const& subscriber : retired)
    {
      subscriber.slotPtr->retire();
    }

    if (needsFinals)
    {
      statePtr->dispatcherPtr->dispatch([&]
                                        { statePtr->publications.invoke([&] { statePtr->deliverFinals(retired); }); });
    }

    // An external caller also waits for self-cancelled or self-retired
    // publications to unwind. No unrelated dispatcher turn is needed to wait.
    statePtr->publications.retire();
  }
} // namespace ao::audio::backend::detail
