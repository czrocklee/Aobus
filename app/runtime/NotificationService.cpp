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

    bool textFits(std::string_view const text, NotificationFeedLimits const& limits) noexcept
    {
      return text.size() <= limits.maxTextBytes;
    }

    void canonicalizeStorage(std::string& text)
    {
      text.shrink_to_fit();
    }

    void canonicalizeStorage(NotificationProgressState& progress)
    {
      canonicalizeStorage(progress.label);
    }

    void canonicalizeStorage(NotificationContentState& content)
    {
      canonicalizeStorage(content.templateId);
      canonicalizeStorage(content.title);
      canonicalizeStorage(content.iconName);

      for (auto& action : content.actions)
      {
        canonicalizeStorage(action.id);
        canonicalizeStorage(action.label);
      }

      content.actions.shrink_to_fit();

      if (content.optProgress)
      {
        canonicalizeStorage(*content.optProgress);
      }
    }

    void canonicalizeStorage(std::optional<NotificationProgressState>& optProgress)
    {
      if (optProgress)
      {
        canonicalizeStorage(*optProgress);
      }
    }

    void canonicalizeStorage(NotificationEntry& entry)
    {
      if (entry.optReportKey)
      {
        canonicalizeStorage(entry.optReportKey->raw());
      }

      canonicalizeStorage(entry.message);
      canonicalizeStorage(entry.content);
    }

    bool contentFits(NotificationContentState const& content, NotificationFeedLimits const& limits) noexcept
    {
      if (!textFits(content.templateId, limits) || !textFits(content.title, limits) ||
          !textFits(content.iconName, limits) || content.actions.size() > limits.maxActionsPerEntry)
      {
        return false;
      }

      if (content.optProgress && !textFits(content.optProgress->label, limits))
      {
        return false;
      }

      return std::ranges::all_of(content.actions,
                                 [&](NotificationAction const& action)
                                 { return textFits(action.id, limits) && textFits(action.label, limits); });
    }

    bool lifetimeFits(NotificationLifetime const lifetime) noexcept
    {
      auto const optDuration = lifetime.optTransientDuration();
      return !optDuration || *optDuration > std::chrono::milliseconds::zero();
    }

    bool entryFits(NotificationEntry const& entry, NotificationFeedLimits const& limits) noexcept
    {
      return (!entry.optReportKey || (!entry.optReportKey->empty() && textFits(entry.optReportKey->raw(), limits))) &&
             textFits(entry.message, limits) && contentFits(entry.content, limits) && lifetimeFits(entry.lifetime);
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
      if ((entry.optReportKey && !consumeText(entry.optReportKey->raw(), remainingBytes)) ||
          !consumeText(entry.message, remainingBytes) || !consumeText(entry.content.templateId, remainingBytes) ||
          !consumeText(entry.content.title, remainingBytes) || !consumeText(entry.content.iconName, remainingBytes))
      {
        return false;
      }

      for (auto const& action : entry.content.actions)
      {
        if (!consumeText(action.id, remainingBytes) || !consumeText(action.label, remainingBytes))
        {
          return false;
        }
      }

      return !entry.content.optProgress || consumeText(entry.content.optProgress->label, remainingBytes);
    }

    NotificationEntry entryFromRequest(NotificationId const id,
                                       std::optional<NotificationReportKey> optReportKey,
                                       NotificationRequest request,
                                       std::uint64_t const lifetimeGeneration = 0)
    {
      return NotificationEntry{
        .id = id,
        .optReportKey = std::move(optReportKey),
        .severity = request.severity,
        .message = std::move(request.message),
        .lifetime = request.lifetime,
        .lifetimeGeneration = lifetimeGeneration,
        .activityPresentation = request.activityPresentation,
        .content = std::move(request.content),
      };
    }

    std::string_view mutationName(NotificationFeedMutationKind const mutationKind) noexcept
    {
      switch (mutationKind)
      {
        case NotificationFeedMutationKind::Posted: return "post";
        case NotificationFeedMutationKind::ReportUpdated: return "report update";
        case NotificationFeedMutationKind::MessageUpdated: return "message update";
        case NotificationFeedMutationKind::ContentUpdated: return "content update";
        case NotificationFeedMutationKind::ProgressUpdated: return "progress update";
        case NotificationFeedMutationKind::ProgressCleared: return "progress clear";
        case NotificationFeedMutationKind::Expired: return "expiry";
        case NotificationFeedMutationKind::Dismissed: return "dismissal";
        case NotificationFeedMutationKind::Cleared: return "clear";
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
                std::vector<NotificationId> const& evictedIds,
                std::uint64_t const committedNextId)
    {
      ++candidatePtr->revision;
      auto immutableFeedPtr = std::shared_ptr<NotificationFeedState const>{std::move(candidatePtr)};
      auto update = NotificationFeedUpdate{
        .revision = immutableFeedPtr->revision,
        .mutationKind = mutationKind,
        .affectedIds = std::move(affectedIds),
        .evictedIds = evictedIds,
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

    async::TaskHandle prepareExpiryAfterMutation(NotificationEntry& entry)
    {
      auto const optDuration = entry.lifetime.optTransientDuration();

      if (!optDuration)
      {
        return {};
      }

      ++entry.lifetimeGeneration;
      return scheduleExpiry(entry.id, entry.lifetimeGeneration, *optDuration);
    }

    void installPreparedExpiry(NotificationId const id, std::uint64_t const generation, async::TaskHandle expiryTask)
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

    bool feedFitsBounds(NotificationFeedState const& candidate) const noexcept
    {
      if (candidate.entries.size() > limits.maxEntries)
      {
        return false;
      }

      auto const historyCount =
        std::ranges::count_if(candidate.entries,
                              [](NotificationEntry const& entry)
                              { return entry.lifetime.kind() == NotificationLifetimeKind::SessionHistory; });

      if (std::cmp_greater(historyCount, limits.maxSessionHistoryEntries))
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
          { return entry.id != protectedId && entry.lifetime.kind() == NotificationLifetimeKind::SessionHistory; });

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
        expiryTasks.erase(id);
      }
    }

    NotificationMutationReply rejectMutation(NotificationFeedMutationKind const mutationKind,
                                             NotificationId const id,
                                             std::string_view const reason) const
    {
      APP_LOG_ERROR("NotificationService rejected {} for notification {}: {}", mutationName(mutationKind), id, reason);
      return {.outcome = NotificationMutationOutcome::Rejected, .id = id};
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
      commit(std::move(candidatePtr), NotificationFeedMutationKind::Expired, {id}, {}, nextId);
      expiryTasks.erase(id);
    }

    NotificationMutationReply commitCandidateMutation(std::shared_ptr<NotificationFeedState> candidatePtr,
                                                      NotificationId const id,
                                                      NotificationFeedMutationKind const mutationKind)
    {
      auto candidateEntryIter = std::ranges::find(candidatePtr->entries, id, &NotificationEntry::id);
      gsl_Expects(candidateEntryIter != candidatePtr->entries.end());

      if (!entryFits(*candidateEntryIter, limits))
      {
        return rejectMutation(mutationKind, id, "candidate violates request limits");
      }

      if (candidateEntryIter->lifetime.kind() == NotificationLifetimeKind::Transient &&
          candidateEntryIter->lifetimeGeneration == std::numeric_limits<std::uint64_t>::max())
      {
        return rejectMutation(mutationKind, id, "lifetime generation is exhausted");
      }

      auto optEvictedIds = evictHistoryToFit(*candidatePtr, id);

      if (!optEvictedIds)
      {
        return rejectMutation(mutationKind, id, "feed capacity is exhausted by non-evictable entries");
      }

      candidateEntryIter = std::ranges::find(candidatePtr->entries, id, &NotificationEntry::id);
      gsl_Expects(candidateEntryIter != candidatePtr->entries.end());
      auto expiryTask = prepareExpiryAfterMutation(*candidateEntryIter);
      auto const generation = candidateEntryIter->lifetimeGeneration;

      commit(std::move(candidatePtr), mutationKind, {id}, *optEvictedIds, nextId);
      eraseExpiryRegistrations(*optEvictedIds);
      installPreparedExpiry(id, generation, std::move(expiryTask));
      return {.outcome = NotificationMutationOutcome::Applied, .id = id};
    }

    template<typename Value, typename Accessor>
    NotificationMutationReply replaceEntryValue(NotificationId const id,
                                                NotificationFeedMutationKind const mutationKind,
                                                Value&& value,
                                                Accessor accessor)
    {
      ensureOnExecutor();
      auto const& entries = feedPtr->entries;
      auto const entryIter = std::ranges::find(entries, id, &NotificationEntry::id);

      if (entryIter == entries.end())
      {
        return {.outcome = NotificationMutationOutcome::Missing, .id = id};
      }

      if (std::invoke(accessor, *entryIter) == value)
      {
        return {.outcome = NotificationMutationOutcome::Unchanged, .id = id};
      }

      canonicalizeStorage(value);
      auto candidatePtr = mutableFeedCopy();
      auto& candidateEntry = candidatePtr->entries[static_cast<std::size_t>(entryIter - entries.begin())];
      std::invoke(accessor, candidateEntry) = std::forward<Value>(value);
      return commitCandidateMutation(std::move(candidatePtr), id, mutationKind);
    }

    NotificationMutationReply replaceProgress(NotificationId const id,
                                              NotificationFeedMutationKind const mutationKind,
                                              std::optional<NotificationProgressState> optProgress)
    {
      return replaceEntryValue(id,
                               mutationKind,
                               std::move(optProgress),
                               [](auto& entry) -> decltype(auto) { return (entry.content.optProgress); });
    }

    NotificationMutationReply post(std::optional<NotificationReportKey> optReportKey, NotificationRequest request)
    {
      ensureOnExecutor();

      if (nextId == std::numeric_limits<std::uint64_t>::max())
      {
        return rejectMutation(
          NotificationFeedMutationKind::Posted, kInvalidNotificationId, "notification id space is exhausted");
      }

      auto const committedNextId = nextId + 1;
      auto const id = NotificationId{committedNextId};
      auto entry = entryFromRequest(id, std::move(optReportKey), std::move(request));

      if (!entryFits(entry, limits))
      {
        return rejectMutation(NotificationFeedMutationKind::Posted, kInvalidNotificationId, "request violates limits");
      }

      canonicalizeStorage(entry);
      auto candidatePtr = mutableFeedCopy();
      candidatePtr->entries.push_back(std::move(entry));
      auto optEvictedIds = evictHistoryToFit(*candidatePtr, id);

      if (!optEvictedIds)
      {
        return rejectMutation(NotificationFeedMutationKind::Posted,
                              kInvalidNotificationId,
                              "feed capacity is exhausted by non-evictable entries");
      }

      auto& candidateEntry = candidatePtr->entries.back();
      auto expiryTask = prepareExpiryAfterMutation(candidateEntry);
      auto const [taskIter, inserted] = expiryTasks.try_emplace(id, std::move(expiryTask));
      std::ignore = taskIter;
      gsl_Expects(inserted);

      try
      {
        commit(std::move(candidatePtr), NotificationFeedMutationKind::Posted, {id}, *optEvictedIds, committedNextId);
      }
      catch (...)
      {
        expiryTasks.erase(id);
        throw;
      }

      eraseExpiryRegistrations(*optEvictedIds);
      return {.outcome = NotificationMutationOutcome::Applied, .id = id};
    }

    async::Runtime& runtime;
    async::Executor& executor;
    NotificationFeedLimits limits;
    std::shared_ptr<NotificationFeedState const> feedPtr;
    std::uint64_t nextId = 0;
    std::deque<NotificationFeedUpdate> pendingUpdates;
    bool publishing = false;
    async::Signal<NotificationFeedUpdate const&> feedUpdatedSignal;
    // Every live entry owns one slot. Retained entries hold an empty handle so
    // a keyed lifetime transition never needs to allocate after feed commit.
    std::unordered_map<NotificationId, async::TaskHandle> expiryTasks;
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

  NotificationMutationReply NotificationService::post(NotificationSeverity const severity,
                                                      std::string message,
                                                      NotificationLifetime const lifetime)
  {
    return post(NotificationRequest{
      .severity = severity,
      .message = std::move(message),
      .lifetime = lifetime,
    });
  }

  NotificationMutationReply NotificationService::post(NotificationRequest request)
  {
    return _implPtr->post(std::nullopt, std::move(request));
  }

  NotificationMutationReply NotificationService::createOrUpdate(NotificationReportKey reportKey,
                                                                NotificationRequest request)
  {
    _implPtr->ensureOnExecutor();
    auto const& entries = _implPtr->feedPtr->entries;
    auto const entryIter =
      std::ranges::find_if(entries, [&](NotificationEntry const& entry) { return entry.optReportKey == reportKey; });

    if (entryIter == entries.end())
    {
      return _implPtr->post(std::optional{std::move(reportKey)}, std::move(request));
    }

    auto const id = entryIter->id;
    auto replacement =
      entryFromRequest(id, std::optional{std::move(reportKey)}, std::move(request), entryIter->lifetimeGeneration);

    if (!entryFits(replacement, _implPtr->limits))
    {
      return _implPtr->rejectMutation(NotificationFeedMutationKind::ReportUpdated, id, "request violates limits");
    }

    if (*entryIter == replacement)
    {
      return {.outcome = NotificationMutationOutcome::Unchanged, .id = id};
    }

    canonicalizeStorage(replacement);
    auto candidatePtr = _implPtr->mutableFeedCopy();
    candidatePtr->entries[static_cast<std::size_t>(entryIter - entries.begin())] = std::move(replacement);
    return _implPtr->commitCandidateMutation(std::move(candidatePtr), id, NotificationFeedMutationKind::ReportUpdated);
  }

  NotificationMutationReply NotificationService::updateMessage(NotificationId const id, std::string message)
  {
    return _implPtr->replaceEntryValue(id,
                                       NotificationFeedMutationKind::MessageUpdated,
                                       std::move(message),
                                       [](auto& entry) -> decltype(auto) { return (entry.message); });
  }

  NotificationMutationReply NotificationService::updateContent(NotificationId const id,
                                                               NotificationContentState content)
  {
    return _implPtr->replaceEntryValue(id,
                                       NotificationFeedMutationKind::ContentUpdated,
                                       std::move(content),
                                       [](auto& entry) -> decltype(auto) { return (entry.content); });
  }

  NotificationMutationReply NotificationService::updateProgress(NotificationId const id,
                                                                NotificationProgressState progress)
  {
    return _implPtr->replaceProgress(
      id, NotificationFeedMutationKind::ProgressUpdated, std::optional{std::move(progress)});
  }

  NotificationMutationReply NotificationService::clearProgress(NotificationId const id)
  {
    return _implPtr->replaceProgress(
      id, NotificationFeedMutationKind::ProgressCleared, std::optional<NotificationProgressState>{});
  }

  NotificationMutationReply NotificationService::dismiss(NotificationId const id)
  {
    _implPtr->ensureOnExecutor();
    auto const& entries = _implPtr->feedPtr->entries;
    auto const entryIter = std::ranges::find(entries, id, &NotificationEntry::id);

    if (entryIter == entries.end())
    {
      return {.outcome = NotificationMutationOutcome::Missing, .id = id};
    }

    auto candidatePtr = _implPtr->mutableFeedCopy();
    candidatePtr->entries.erase(candidatePtr->entries.begin() + (entryIter - entries.begin()));
    _implPtr->commit(std::move(candidatePtr), NotificationFeedMutationKind::Dismissed, {id}, {}, _implPtr->nextId);
    _implPtr->expiryTasks.erase(id);
    return {.outcome = NotificationMutationOutcome::Applied, .id = id};
  }

  NotificationMutationReply NotificationService::dismissAll()
  {
    _implPtr->ensureOnExecutor();

    if (_implPtr->feedPtr->entries.empty())
    {
      return {.outcome = NotificationMutationOutcome::Unchanged};
    }

    auto affectedIds = std::vector<NotificationId>{};
    affectedIds.reserve(_implPtr->feedPtr->entries.size());
    std::ranges::transform(_implPtr->feedPtr->entries, std::back_inserter(affectedIds), &NotificationEntry::id);

    auto candidatePtr = _implPtr->mutableFeedCopy();
    candidatePtr->entries.clear();
    _implPtr->commit(std::move(candidatePtr), NotificationFeedMutationKind::Cleared, affectedIds, {}, _implPtr->nextId);
    _implPtr->eraseExpiryRegistrations(affectedIds);
    return {.outcome = NotificationMutationOutcome::Applied};
  }
} // namespace ao::rt
