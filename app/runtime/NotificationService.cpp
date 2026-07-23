// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/async/Executor.h>
#include <ao/async/Runtime.h>
#include <ao/async/Signal.h>
#include <ao/async/Subscription.h>
#include <ao/async/Task.h>
#include <ao/rt/Log.h>
#include <ao/rt/NotificationIds.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>

#include <gsl-lite/gsl-lite.hpp>

#include <algorithm>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <source_location>
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace ao::rt
{
  namespace
  {
    [[noreturn]] void failExecutorAffinity(std::source_location const& location)
    {
      APP_LOG_CRITICAL("NotificationService thread-affinity violation: '{}' invoked off the executor thread ({}:{})",
                       location.function_name(),
                       location.file_name(),
                       location.line());

      if (auto const& loggerPtr = Log::appLogger(); loggerPtr)
      {
        loggerPtr->flush();
      }

      std::abort();
    }

    bool textFits(std::string_view const text, NotificationFeedLimits const& limits) noexcept
    {
      return text.size() <= limits.maxTextBytes;
    }

    void canonicalizeStorage(std::string& text)
    {
      text.shrink_to_fit();
    }

    void canonicalizeStorage(NotificationMessage& message)
    {
      std::visit(
        []<typename Message>(Message& value)
        {
          if constexpr (std::same_as<Message, std::string>)
          {
            canonicalizeStorage(value);
          }
          else
          {
            canonicalizeStorage(value.subject);
            canonicalizeStorage(value.detail);
          }
        },
        message);
    }

    void canonicalizeStorage(NotificationEntry& entry)
    {
      if (entry.optReportKey)
      {
        canonicalizeStorage(entry.optReportKey->raw());
      }

      canonicalizeStorage(entry.message);
    }

    bool messageFits(NotificationMessage const& message, NotificationFeedLimits const& limits) noexcept
    {
      return std::visit(
        [&limits]<typename Message>(Message const& value)
        {
          if constexpr (std::same_as<Message, std::string>)
          {
            return textFits(value, limits);
          }
          else
          {
            return textFits(value.subject, limits) && textFits(value.detail, limits);
          }
        },
        message);
    }

    bool lifetimeFits(NotificationLifetime const lifetime) noexcept
    {
      auto const optDuration = lifetime.optTransientDuration();
      return !optDuration || *optDuration > std::chrono::milliseconds::zero();
    }

    bool entryFits(NotificationEntry const& entry, NotificationFeedLimits const& limits) noexcept
    {
      return (!entry.optReportKey || (!entry.optReportKey->empty() && textFits(entry.optReportKey->raw(), limits))) &&
             messageFits(entry.message, limits) && lifetimeFits(entry.lifetime);
    }

    bool consumeText(std::string_view const text, std::size_t& remainingBytes) noexcept
    {
      if (text.size() > remainingBytes)
      {
        return false;
      }

      remainingBytes -= text.size();
      return true;
    }

    bool consumeEntryText(NotificationEntry const& entry, std::size_t& remainingBytes) noexcept
    {
      auto const consumeMessage = [&remainingBytes](NotificationMessage const& message)
      {
        return std::visit(
          [&remainingBytes]<typename Message>(Message const& value)
          {
            if constexpr (std::same_as<Message, std::string>)
            {
              return consumeText(value, remainingBytes);
            }
            else
            {
              return consumeText(value.subject, remainingBytes) && consumeText(value.detail, remainingBytes);
            }
          },
          message);
      };

      return (!entry.optReportKey || consumeText(entry.optReportKey->raw(), remainingBytes)) &&
             consumeMessage(entry.message);
    }

    NotificationEntry entryFromRequest(NotificationId const id,
                                       std::optional<NotificationReportKey> optReportKey,
                                       NotificationRequest request)
    {
      return NotificationEntry{
        .id = id,
        .optReportKey = std::move(optReportKey),
        .severity = request.severity,
        .message = std::move(request.message),
        .lifetime = request.lifetime,
      };
    }

    std::string_view mutationName(NotificationFeedMutationKind const mutationKind) noexcept
    {
      switch (mutationKind)
      {
        case NotificationFeedMutationKind::Posted: return "post";
        case NotificationFeedMutationKind::ReportUpdated: return "report update";
        case NotificationFeedMutationKind::Expired: return "expiry";
      }

      return "unknown";
    }
  } // namespace

  struct NotificationService::Impl final
  {
    struct ExpiryControl final
    {
      explicit ExpiryControl(Impl* const serviceImpl)
        : owner{serviceImpl}
      {
      }

      Impl* owner;
    };

    struct ExpiryRegistration final
    {
      async::TaskHandle task{};
    };

    explicit Impl(async::Runtime& asyncRuntime, NotificationFeedLimits feedLimits)
      : runtime{asyncRuntime}
      , executor{asyncRuntime.callbackExecutor()}
      , limits{feedLimits}
      , feedPtr{std::make_shared<NotificationFeedState const>()}
      , expiryControlPtr{std::make_shared<ExpiryControl>(this)}
    {
    }

    ~Impl()
    {
      // Queued expiry callbacks retain only this weak control block. Retire it
      // before cancellation so a timer that already won the race becomes a
      // callback-executor no-op instead of borrowing a destroyed service.
      expiryControlPtr.reset();
      expiryRegistrations.clear();
    }

    Impl(Impl const&) = delete;
    Impl& operator=(Impl const&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    void ensureOnExecutor(std::source_location location = std::source_location::current()) const
    {
      if (!executor.isCurrent()) [[unlikely]]
      {
        failExecutorAffinity(location);
      }
    }

    std::shared_ptr<NotificationFeedState> mutableFeedCopy() const
    {
      return std::make_shared<NotificationFeedState>(*feedPtr);
    }

    void commit(std::shared_ptr<NotificationFeedState> candidatePtr,
                NotificationFeedMutationKind const mutationKind,
                NotificationId const id,
                std::uint64_t const committedNextId)
    {
      auto immutableFeedPtr = std::shared_ptr<NotificationFeedState const>{std::move(candidatePtr)};
      auto update = NotificationFeedUpdate{
        .mutationKind = mutationKind,
        .id = id,
        .feedPtr = immutableFeedPtr,
      };

      // Queue allocation is the final operation that may fail before commit.
      // Once the update is queued, the new snapshot and id watermark become
      // authoritative together and observer delivery cannot roll them back.
      pendingUpdates.push_back(std::move(update));
      feedPtr = std::move(immutableFeedPtr);
      nextId = committedNextId;
      drainPendingUpdates();
    }

    void drainPendingUpdates() noexcept
    {
      if (publishing)
      {
        return;
      }

      publishing = true;

      while (!pendingUpdates.empty())
      {
        auto update = std::move(pendingUpdates.front());
        pendingUpdates.pop_front();

        try
        {
          feedUpdatedSignal.emit(update);
        }
        catch (...)
        {
          reportObserverFailure(std::current_exception());
        }
      }

      publishing = false;
    }

    void reportObserverFailure(std::exception_ptr exceptionPtr) const noexcept
    {
      runtime.reportUnhandledException(std::move(exceptionPtr), "notification feed observer");
    }

    static async::Task<void> waitForExpiry(async::Runtime* runtime,
                                           std::weak_ptr<ExpiryControl> expiryControlWeakPtr,
                                           std::weak_ptr<ExpiryRegistration> expiryRegistrationWeakPtr,
                                           NotificationId const id,
                                           std::chrono::milliseconds const duration,
                                           std::stop_token const stopToken)
    {
      co_await runtime->sleepFor(duration, stopToken);
      co_await runtime->resumeOnCallbackExecutor(stopToken);

      auto const controlPtr = expiryControlWeakPtr.lock();
      auto const registrationPtr = expiryRegistrationWeakPtr.lock();

      if (controlPtr && registrationPtr)
      {
        controlPtr->owner->expireTransient(id, registrationPtr);
      }
    }

    async::TaskHandle scheduleExpiry(NotificationId const id,
                                     std::weak_ptr<ExpiryRegistration> expiryRegistrationWeakPtr,
                                     std::chrono::milliseconds const duration)
    {
      gsl_Expects(duration > std::chrono::milliseconds::zero());
      return runtime.spawnCancellable(
        [runtime = &runtime,
         expiryControlWeakPtr = std::weak_ptr{expiryControlPtr},
         expiryRegistrationWeakPtr = std::move(expiryRegistrationWeakPtr),
         id,
         duration](std::stop_token const stopToken)
        { return waitForExpiry(runtime, expiryControlWeakPtr, expiryRegistrationWeakPtr, id, duration, stopToken); });
    }

    std::shared_ptr<ExpiryRegistration> prepareExpiry(NotificationEntry const& entry)
    {
      auto registrationPtr = std::make_shared<ExpiryRegistration>();
      auto const optDuration = entry.lifetime.optTransientDuration();

      if (optDuration)
      {
        registrationPtr->task = scheduleExpiry(entry.id, registrationPtr, *optDuration);
      }

      return registrationPtr;
    }

    bool feedFitsBounds(NotificationFeedState const& candidate) const noexcept
    {
      if (candidate.entries.size() > limits.maxEntries)
      {
        return false;
      }

      auto const historyCount = std::ranges::count_if(
        candidate.entries,
        [](NotificationEntry const& entry) { return entry.lifetime.kind() == NotificationLifetimeKind::History; });

      if (std::cmp_greater(historyCount, limits.maxHistoryEntries))
      {
        return false;
      }

      auto remainingBytes = limits.maxTotalTextBytes;
      return std::ranges::all_of(
        candidate.entries, [&](NotificationEntry const& entry) { return consumeEntryText(entry, remainingBytes); });
    }

    std::optional<std::vector<NotificationId>> evictHistoryToFit(NotificationFeedState& candidate,
                                                                 NotificationId const protectedId) const
    {
      auto evictedIds = std::vector<NotificationId>{};

      while (!feedFitsBounds(candidate))
      {
        auto const entryIter = std::ranges::find_if(
          candidate.entries,
          [&](NotificationEntry const& entry)
          { return entry.id != protectedId && entry.lifetime.kind() == NotificationLifetimeKind::History; });

        if (entryIter == candidate.entries.end())
        {
          return std::nullopt;
        }

        evictedIds.push_back(entryIter->id);
        candidate.entries.erase(entryIter);
      }

      return evictedIds;
    }

    void eraseExpiryRegistrations(std::vector<NotificationId> const& ids)
    {
      for (auto const id : ids)
      {
        expiryRegistrations.erase(id);
      }
    }

    void rejectMutation(NotificationFeedMutationKind const mutationKind,
                        NotificationId const id,
                        std::string_view const reason) const
    {
      APP_LOG_ERROR("NotificationService rejected {} for notification {}: {}", mutationName(mutationKind), id, reason);
    }

    void expireTransient(NotificationId const id, std::shared_ptr<ExpiryRegistration> const& registrationPtr)
    {
      ensureOnExecutor();
      auto const& entries = feedPtr->entries;
      auto const entryIter = std::ranges::find(entries, id, &NotificationEntry::id);

      if (auto const registrationIter = expiryRegistrations.find(id);
          entryIter == entries.end() || entryIter->lifetime.kind() != NotificationLifetimeKind::Transient ||
          registrationIter == expiryRegistrations.end() || registrationIter->second != registrationPtr)
      {
        return;
      }

      auto candidatePtr = mutableFeedCopy();
      candidatePtr->entries.erase(candidatePtr->entries.begin() + (entryIter - entries.begin()));
      commit(std::move(candidatePtr), NotificationFeedMutationKind::Expired, id, nextId);

      if (auto const registrationIter = expiryRegistrations.find(id);
          registrationIter != expiryRegistrations.end() && registrationIter->second == registrationPtr)
      {
        expiryRegistrations.erase(registrationIter);
      }
    }

    void commitCandidateMutation(std::shared_ptr<NotificationFeedState> candidatePtr,
                                 NotificationId const id,
                                 NotificationFeedMutationKind const mutationKind)
    {
      auto candidateEntryIter = std::ranges::find(candidatePtr->entries, id, &NotificationEntry::id);
      gsl_Expects(candidateEntryIter != candidatePtr->entries.end());

      if (!entryFits(*candidateEntryIter, limits))
      {
        rejectMutation(mutationKind, id, "candidate violates request limits");
        return;
      }

      auto const registrationIter = expiryRegistrations.find(id);
      gsl_Expects(registrationIter != expiryRegistrations.end());
      auto previousRegistrationPtr = registrationIter->second;

      auto optEvictedIds = evictHistoryToFit(*candidatePtr, id);

      if (!optEvictedIds)
      {
        rejectMutation(mutationKind, id, "feed capacity is exhausted by non-evictable entries");
        return;
      }

      candidateEntryIter = std::ranges::find(candidatePtr->entries, id, &NotificationEntry::id);
      gsl_Expects(candidateEntryIter != candidatePtr->entries.end());
      auto candidateRegistrationPtr = prepareExpiry(*candidateEntryIter);

      // Install the candidate identity before synchronous publication so a
      // reentrant update supersedes this exact registration.
      registrationIter->second = candidateRegistrationPtr;

      try
      {
        commit(std::move(candidatePtr), mutationKind, id, nextId);
      }
      catch (...)
      {
        if (auto const currentIter = expiryRegistrations.find(id);
            currentIter != expiryRegistrations.end() && currentIter->second == candidateRegistrationPtr)
        {
          currentIter->second = std::move(previousRegistrationPtr);
        }

        throw;
      }

      eraseExpiryRegistrations(*optEvictedIds);
    }

    void post(std::optional<NotificationReportKey> optReportKey, NotificationRequest request)
    {
      ensureOnExecutor();

      if (nextId == std::numeric_limits<std::uint64_t>::max())
      {
        rejectMutation(
          NotificationFeedMutationKind::Posted, kInvalidNotificationId, "notification id space is exhausted");
        return;
      }

      auto const committedNextId = nextId + 1;
      auto const id = NotificationId{committedNextId};
      auto entry = entryFromRequest(id, std::move(optReportKey), std::move(request));

      if (!entryFits(entry, limits))
      {
        rejectMutation(NotificationFeedMutationKind::Posted, kInvalidNotificationId, "request violates limits");
        return;
      }

      canonicalizeStorage(entry);
      auto candidatePtr = mutableFeedCopy();
      candidatePtr->entries.push_back(std::move(entry));
      auto optEvictedIds = evictHistoryToFit(*candidatePtr, id);

      if (!optEvictedIds)
      {
        rejectMutation(NotificationFeedMutationKind::Posted,
                       kInvalidNotificationId,
                       "feed capacity is exhausted by non-evictable entries");
        return;
      }

      auto& candidateEntry = candidatePtr->entries.back();
      auto expiryRegistrationPtr = prepareExpiry(candidateEntry);
      auto const [registrationIter, inserted] = expiryRegistrations.try_emplace(id, expiryRegistrationPtr);
      gsl_Expects(inserted);

      try
      {
        commit(std::move(candidatePtr), NotificationFeedMutationKind::Posted, id, committedNextId);
      }
      catch (...)
      {
        if (registrationIter->second == expiryRegistrationPtr)
        {
          expiryRegistrations.erase(registrationIter);
        }

        throw;
      }

      eraseExpiryRegistrations(*optEvictedIds);
    }

    async::Runtime& runtime;
    async::Executor& executor;
    NotificationFeedLimits limits;
    std::shared_ptr<NotificationFeedState const> feedPtr;
    std::uint64_t nextId = 0;
    std::deque<NotificationFeedUpdate> pendingUpdates;
    bool publishing = false;
    async::Signal<NotificationFeedUpdate const&> feedUpdatedSignal;
    // Every live entry owns one slot. Retained entries hold an empty task so a
    // keyed lifetime transition never needs to allocate control state after commit.
    std::unordered_map<NotificationId, std::shared_ptr<ExpiryRegistration>> expiryRegistrations;
    std::shared_ptr<ExpiryControl> expiryControlPtr;
  };

  NotificationService::NotificationService(async::Runtime& runtime, NotificationFeedLimits limits)
    : _implPtr{std::make_unique<Impl>(runtime, limits)}
  {
  }

  NotificationService::~NotificationService() = default;

  async::Subscription NotificationService::onFeedUpdated(
    std::move_only_function<void(NotificationFeedUpdate const&)> handler)
  {
    _implPtr->ensureOnExecutor();
    return _implPtr->feedUpdatedSignal.connect(std::move(handler));
  }

  NotificationFeedState NotificationService::feed() const
  {
    _implPtr->ensureOnExecutor();
    return *_implPtr->feedPtr;
  }

  void NotificationService::post(NotificationSeverity const severity,
                                 std::string message,
                                 NotificationLifetime const lifetime)
  {
    post(NotificationRequest{
      .severity = severity,
      .message = std::move(message),
      .lifetime = lifetime,
    });
  }

  void NotificationService::post(NotificationRequest request)
  {
    _implPtr->post(std::nullopt, std::move(request));
  }

  void NotificationService::createOrUpdate(NotificationReportKey reportKey, NotificationRequest request)
  {
    _implPtr->ensureOnExecutor();
    auto const& entries = _implPtr->feedPtr->entries;
    auto const entryIter =
      std::ranges::find_if(entries, [&](NotificationEntry const& entry) { return entry.optReportKey == reportKey; });

    if (entryIter == entries.end())
    {
      _implPtr->post(std::optional{std::move(reportKey)}, std::move(request));
      return;
    }

    auto const id = entryIter->id;
    auto replacement = entryFromRequest(id, std::optional{std::move(reportKey)}, std::move(request));

    if (!entryFits(replacement, _implPtr->limits))
    {
      _implPtr->rejectMutation(NotificationFeedMutationKind::ReportUpdated, id, "request violates limits");
      return;
    }

    if (*entryIter == replacement)
    {
      return;
    }

    canonicalizeStorage(replacement);
    auto candidatePtr = _implPtr->mutableFeedCopy();
    candidatePtr->entries[static_cast<std::size_t>(entryIter - entries.begin())] = std::move(replacement);
    _implPtr->commitCandidateMutation(std::move(candidatePtr), id, NotificationFeedMutationKind::ReportUpdated);
  }
} // namespace ao::rt
