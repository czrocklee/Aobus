// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/async/AsyncExceptionHandler.h>
#include <ao/async/Executor.h>
#include <ao/rt/Log.h>
#include <ao/rt/NotificationIds.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/Signal.h>
#include <ao/rt/Subscription.h>

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
#include <string>
#include <string_view>
#include <system_error>
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
    explicit Impl(async::Executor& callbackExecutor, async::AsyncExceptionHandler exceptionHandler)
      : executor{callbackExecutor}
      , observerExceptionHandler{std::move(exceptionHandler)}
      , feedPtr{std::make_shared<NotificationFeedState const>()}
    {
    }

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
      if (!observerExceptionHandler)
      {
        return;
      }

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

      try
      {
        observerExceptionHandler(std::move(exceptionPtr), context);
      }
      // NOLINTNEXTLINE(bugprone-empty-catch): A diagnostic handler must not escape the committed publication boundary.
      catch (...)
      {
      }
    }

    async::Executor& executor;
    async::AsyncExceptionHandler observerExceptionHandler;
    std::shared_ptr<NotificationFeedState const> feedPtr;
    std::uint64_t nextId = 0;
    std::deque<NotificationFeedUpdate> pendingUpdates;
    bool publishing = false;
    Signal<NotificationFeedUpdate const&> feedUpdatedSignal;
  };

  NotificationService::NotificationService(async::Executor& executor,
                                           async::AsyncExceptionHandler observerExceptionHandler)
    : _implPtr{std::make_unique<Impl>(executor, std::move(observerExceptionHandler))}
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
                                           bool const sticky,
                                           std::optional<std::chrono::milliseconds> const optTimeout)
  {
    return post(NotificationRequest{
      .severity = severity,
      .message = std::move(message),
      .sticky = sticky,
      .optTimeout = optTimeout,
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
      .sticky = request.sticky,
      .optTimeout = request.optTimeout,
      .activityPresentation = request.activityPresentation,
      .content = std::move(request.content),
    };

    auto candidatePtr = _implPtr->mutableFeedCopy();
    candidatePtr->entries.push_back(std::move(entry));
    _implPtr->commit(std::move(candidatePtr), NotificationFeedMutationKind::Posted, {id}, committedNextId);

    return id;
  }

  bool NotificationService::updateMessage(NotificationId const id, std::string message)
  {
    _implPtr->ensureOnExecutor();
    auto const& entries = _implPtr->feedPtr->entries;
    auto const it = std::ranges::find(entries, id, &NotificationEntry::id);

    if (it != entries.end())
    {
      auto candidatePtr = _implPtr->mutableFeedCopy();
      candidatePtr->entries[static_cast<std::size_t>(it - entries.begin())].message = std::move(message);
      _implPtr->commit(std::move(candidatePtr), NotificationFeedMutationKind::MessageUpdated, {id}, _implPtr->nextId);
      return true;
    }

    return false;
  }

  void NotificationService::updateContent(NotificationId const id, NotificationContentState content)
  {
    _implPtr->ensureOnExecutor();
    auto const& entries = _implPtr->feedPtr->entries;
    auto const it = std::ranges::find(entries, id, &NotificationEntry::id);

    if (it != entries.end())
    {
      auto candidatePtr = _implPtr->mutableFeedCopy();
      candidatePtr->entries[static_cast<std::size_t>(it - entries.begin())].content = std::move(content);
      _implPtr->commit(std::move(candidatePtr), NotificationFeedMutationKind::ContentUpdated, {id}, _implPtr->nextId);
    }
  }

  void NotificationService::updateProgress(NotificationId const id, NotificationProgressState progress)
  {
    _implPtr->ensureOnExecutor();
    auto const& entries = _implPtr->feedPtr->entries;
    auto const it = std::ranges::find(entries, id, &NotificationEntry::id);

    if (it != entries.end())
    {
      auto candidatePtr = _implPtr->mutableFeedCopy();
      candidatePtr->entries[static_cast<std::size_t>(it - entries.begin())].content.optProgress = std::move(progress);
      _implPtr->commit(std::move(candidatePtr), NotificationFeedMutationKind::ProgressUpdated, {id}, _implPtr->nextId);
    }
  }

  void NotificationService::clearProgress(NotificationId const id)
  {
    _implPtr->ensureOnExecutor();
    auto const& entries = _implPtr->feedPtr->entries;
    auto const it = std::ranges::find(entries, id, &NotificationEntry::id);

    if (it != entries.end() && it->content.optProgress)
    {
      auto candidatePtr = _implPtr->mutableFeedCopy();
      candidatePtr->entries[static_cast<std::size_t>(it - entries.begin())].content.optProgress = std::nullopt;
      _implPtr->commit(std::move(candidatePtr), NotificationFeedMutationKind::ProgressCleared, {id}, _implPtr->nextId);
    }
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
      _implPtr->commit(
        std::move(candidatePtr), NotificationFeedMutationKind::Cleared, std::move(affectedIds), _implPtr->nextId);
    }
  }
} // namespace ao::rt
