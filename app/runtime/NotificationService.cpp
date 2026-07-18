// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/async/Executor.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>
#include <ao/rt/Log.h>
#include <ao/rt/NotificationIds.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/Signal.h>
#include <ao/rt/Subscription.h>

#include <gsl-lite/gsl-lite.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <exception>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <source_location>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
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

    explicit Impl(async::Runtime& asyncRuntime)
      : runtime{asyncRuntime}
      , executor{asyncRuntime.callbackExecutor()}
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
      expiryTasks.clear();
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
                std::vector<NotificationId> affectedIds,
                std::uint64_t const committedNextId)
    {
      ++candidatePtr->revision;
      auto immutableFeedPtr = std::shared_ptr<NotificationFeedState const>{std::move(candidatePtr)};
      auto update = NotificationFeedUpdate{
        .revision = immutableFeedPtr->revision,
        .mutationKind = mutationKind,
        .affectedIds = std::move(affectedIds),
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
          reportObserverFailure(std::current_exception(), update.revision);
        }
      }

      publishing = false;
    }

    void reportObserverFailure(std::exception_ptr exceptionPtr, std::uint64_t const revision) const noexcept
    {
      constexpr auto kContextPrefix = std::string_view{"notification feed observer at revision "};
      constexpr auto kContextBufferSize = kContextPrefix.size() + std::numeric_limits<std::uint64_t>::digits10 + 1;
      auto contextBuffer = std::array<char, kContextBufferSize>{};
      std::ranges::copy(kContextPrefix, contextBuffer.begin());
      auto* const numberBegin = contextBuffer.data() + kContextPrefix.size();
      auto const result = std::to_chars(numberBegin, contextBuffer.data() + contextBuffer.size(), revision);
      auto const context =
        result.ec == std::errc{}
          ? std::string_view{contextBuffer.data(), static_cast<std::size_t>(result.ptr - contextBuffer.data())}
          : std::string_view{"notification feed observer"};

      runtime.reportUnhandledException(std::move(exceptionPtr), context);
    }

    static async::Task<void> waitForExpiry(async::Runtime* runtime,
                                           async::Executor* executor,
                                           std::weak_ptr<ExpiryControl> expiryControlWeakPtr,
                                           NotificationId const id,
                                           std::uint64_t const generation,
                                           std::chrono::milliseconds const duration,
                                           std::stop_token const stopToken)
    {
      co_await runtime->sleepFor(duration, stopToken);
      executor->defer(
        [expiryControlWeakPtr = std::move(expiryControlWeakPtr), id, generation]
        {
          if (auto const controlPtr = expiryControlWeakPtr.lock(); controlPtr)
          {
            controlPtr->owner->expireTransient(id, generation);
          }
        });
    }

    async::TaskHandle scheduleExpiry(NotificationId const id,
                                     std::uint64_t const generation,
                                     std::chrono::milliseconds const duration)
    {
      gsl_Expects(duration > std::chrono::milliseconds::zero());
      return runtime.spawnCancellable(
        [runtime = &runtime,
         executor = &executor,
         expiryControlWeakPtr = std::weak_ptr{expiryControlPtr},
         id,
         generation,
         duration](std::stop_token const stopToken)
        { return waitForExpiry(runtime, executor, expiryControlWeakPtr, id, generation, duration, stopToken); });
    }

    async::TaskHandle prepareRestartedExpiry(NotificationEntry& entry)
    {
      auto const optDuration = entry.lifetime.optTransientDuration();

      if (!optDuration)
      {
        return {};
      }

      ++entry.lifetimeGeneration;
      return scheduleExpiry(entry.id, entry.lifetimeGeneration, *optDuration);
    }

    void installRestartedExpiry(NotificationId const id, std::uint64_t const generation, async::TaskHandle expiryTask)
    {
      auto const& entries = feedPtr->entries;
      auto const entryIter = std::ranges::find(entries, id, &NotificationEntry::id);

      // Publication is synchronous and reentrant. A nested update or dismissal
      // may already have superseded this timer before the outer command returns.
      if (entryIter == entries.end() || entryIter->lifetimeGeneration != generation)
      {
        return;
      }

      auto taskIter = expiryTasks.find(id);
      gsl_Expects(taskIter != expiryTasks.end());
      taskIter->second = std::move(expiryTask);
    }

    void expireTransient(NotificationId const id, std::uint64_t const generation)
    {
      ensureOnExecutor();
      auto const& entries = feedPtr->entries;
      auto const entryIter = std::ranges::find(entries, id, &NotificationEntry::id);

      if (entryIter == entries.end() || entryIter->lifetime.kind() != NotificationLifetimeKind::Transient ||
          entryIter->lifetimeGeneration != generation)
      {
        return;
      }

      auto candidatePtr = mutableFeedCopy();
      candidatePtr->entries.erase(candidatePtr->entries.begin() + (entryIter - entries.begin()));
      commit(std::move(candidatePtr), NotificationFeedMutationKind::Expired, {id}, nextId);
      expiryTasks.erase(id);
    }

    template<typename Mutator>
    bool mutateEntry(NotificationId const id, NotificationFeedMutationKind const mutationKind, Mutator&& mutator)
    {
      ensureOnExecutor();
      auto const& entries = feedPtr->entries;
      auto const entryIter = std::ranges::find(entries, id, &NotificationEntry::id);

      if (entryIter == entries.end())
      {
        return false;
      }

      auto candidatePtr = mutableFeedCopy();
      auto& candidateEntry = candidatePtr->entries[static_cast<std::size_t>(entryIter - entries.begin())];

      std::invoke(std::forward<Mutator>(mutator), candidateEntry);

      auto expiryTask = prepareRestartedExpiry(candidateEntry);
      auto const generation = candidateEntry.lifetimeGeneration;
      commit(std::move(candidatePtr), mutationKind, {id}, nextId);

      if (expiryTask)
      {
        installRestartedExpiry(id, generation, std::move(expiryTask));
      }

      return true;
    }

    async::Runtime& runtime;
    async::Executor& executor;
    std::shared_ptr<NotificationFeedState const> feedPtr;
    std::uint64_t nextId = 0;
    std::deque<NotificationFeedUpdate> pendingUpdates;
    bool publishing = false;
    Signal<NotificationFeedUpdate const&> feedUpdatedSignal;
    std::unordered_map<NotificationId, async::TaskHandle> expiryTasks;
    std::shared_ptr<ExpiryControl> expiryControlPtr;
  };

  NotificationService::NotificationService(async::Runtime& runtime)
    : _implPtr{std::make_unique<Impl>(runtime)}
  {
  }

  NotificationService::~NotificationService() = default;

  Subscription NotificationService::onFeedUpdated(std::move_only_function<void(NotificationFeedUpdate const&)> handler)
  {
    _implPtr->ensureOnExecutor();
    return _implPtr->feedUpdatedSignal.connect(std::move(handler));
  }

  NotificationFeedState NotificationService::feed() const
  {
    _implPtr->ensureOnExecutor();
    return *_implPtr->feedPtr;
  }

  NotificationId NotificationService::post(NotificationSeverity const severity,
                                           std::string message,
                                           NotificationLifetime const lifetime)
  {
    return post(NotificationRequest{
      .severity = severity,
      .message = std::move(message),
      .lifetime = lifetime,
    });
  }

  NotificationId NotificationService::post(NotificationRequest request)
  {
    _implPtr->ensureOnExecutor();

    auto const committedNextId = _implPtr->nextId + 1;
    auto const id = NotificationId{committedNextId};

    auto entry = NotificationEntry{
      .id = id,
      .severity = request.severity,
      .message = std::move(request.message),
      .lifetime = request.lifetime,
      .activityPresentation = request.activityPresentation,
      .content = std::move(request.content),
    };

    auto candidatePtr = _implPtr->mutableFeedCopy();
    candidatePtr->entries.push_back(std::move(entry));
    auto& candidateEntry = candidatePtr->entries.back();
    auto expiryTask = _implPtr->prepareRestartedExpiry(candidateEntry);

    if (expiryTask)
    {
      auto const [taskIter, inserted] = _implPtr->expiryTasks.try_emplace(id, std::move(expiryTask));
      std::ignore = taskIter;
      gsl_Expects(inserted);

      try
      {
        _implPtr->commit(std::move(candidatePtr), NotificationFeedMutationKind::Posted, {id}, committedNextId);
      }
      catch (...)
      {
        _implPtr->expiryTasks.erase(id);
        throw;
      }
    }
    else
    {
      _implPtr->commit(std::move(candidatePtr), NotificationFeedMutationKind::Posted, {id}, committedNextId);
    }

    return id;
  }

  bool NotificationService::updateMessage(NotificationId const id, std::string message)
  {
    return _implPtr->mutateEntry(id,
                                 NotificationFeedMutationKind::MessageUpdated,
                                 [message = std::move(message)](NotificationEntry& entry) mutable
                                 { entry.message = std::move(message); });
  }

  void NotificationService::updateContent(NotificationId const id, NotificationContentState content)
  {
    std::ignore = _implPtr->mutateEntry(id,
                                        NotificationFeedMutationKind::ContentUpdated,
                                        [content = std::move(content)](NotificationEntry& entry) mutable
                                        { entry.content = std::move(content); });
  }

  void NotificationService::updateProgress(NotificationId const id, NotificationProgressState progress)
  {
    std::ignore = _implPtr->mutateEntry(id,
                                        NotificationFeedMutationKind::ProgressUpdated,
                                        [progress = std::move(progress)](NotificationEntry& entry) mutable
                                        { entry.content.optProgress = std::move(progress); });
  }

  void NotificationService::clearProgress(NotificationId const id)
  {
    _implPtr->ensureOnExecutor();
    auto const& entries = _implPtr->feedPtr->entries;
    auto const entryIter = std::ranges::find(entries, id, &NotificationEntry::id);

    if (entryIter == entries.end() || !entryIter->content.optProgress)
    {
      return;
    }

    std::ignore = _implPtr->mutateEntry(id,
                                        NotificationFeedMutationKind::ProgressCleared,
                                        [](NotificationEntry& entry) { entry.content.optProgress = std::nullopt; });
  }

  void NotificationService::dismiss(NotificationId const id)
  {
    _implPtr->ensureOnExecutor();
    auto const& entries = _implPtr->feedPtr->entries;
    auto const it = std::ranges::find(entries, id, &NotificationEntry::id);

    if (it != entries.end())
    {
      auto candidatePtr = _implPtr->mutableFeedCopy();
      candidatePtr->entries.erase(candidatePtr->entries.begin() + (it - entries.begin()));
      _implPtr->commit(std::move(candidatePtr), NotificationFeedMutationKind::Dismissed, {id}, _implPtr->nextId);
      _implPtr->expiryTasks.erase(id);
    }
  }

  void NotificationService::dismissAll()
  {
    _implPtr->ensureOnExecutor();

    if (!_implPtr->feedPtr->entries.empty())
    {
      auto affectedIds = std::vector<NotificationId>{};
      affectedIds.reserve(_implPtr->feedPtr->entries.size());
      std::ranges::transform(_implPtr->feedPtr->entries, std::back_inserter(affectedIds), &NotificationEntry::id);

      auto candidatePtr = _implPtr->mutableFeedCopy();
      candidatePtr->entries.clear();
      _implPtr->commit(std::move(candidatePtr), NotificationFeedMutationKind::Cleared, affectedIds, _implPtr->nextId);

      for (auto const id : affectedIds)
      {
        _implPtr->expiryTasks.erase(id);
      }
    }
  }
} // namespace ao::rt
