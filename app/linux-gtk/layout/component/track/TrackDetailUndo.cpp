// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TrackDetailUndo.h"

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Task.h>
#include <ao/library/Credits.h>
#include <ao/rt/Log.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/uimodel/library/track/TrackAuthoringSessions.h>

#include <glibmm/main.h>
#include <sigc++/functors/slot.h>
#include <sigc++/signal.h>

#include <algorithm>
#include <bitset>
#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace ao::gtk::layout
{
  namespace
  {
    constexpr auto kUndoTimeout = std::chrono::milliseconds{5000};
  }

  TrackDetailUndoController::TrackDetailUndoController(TrackDetailUndoTimeoutScheduler timeoutScheduler)
    : _timeoutScheduler{std::move(timeoutScheduler)}
  {
  }

  TrackDetailUndoController::~TrackDetailUndoController()
  {
    _presentationCallbacks.close();
    disconnectTimer();
  }

  TrackDetailCustomMetadataUndo const* TrackDetailUndoController::pendingCustomMetadataUndo() const
  {
    return _optPendingUndo ? std::get_if<TrackDetailCustomMetadataUndo>(&*_optPendingUndo) : nullptr;
  }

  TrackDetailCreditsUndo const* TrackDetailUndoController::pendingCreditsUndo() const
  {
    return _optPendingUndo ? std::get_if<TrackDetailCreditsUndo>(&*_optPendingUndo) : nullptr;
  }

  void TrackDetailUndoController::presentCustomMetadataDeletedUndo(std::string key,
                                                                   std::string value,
                                                                   uimodel::TrackAuthoringSession session)
  {
    // A delayed completion can present an offer bound to a session a
    // selection change, replacement, or teardown has already invalidated.
    // That session cannot replay correctly and must not displace a newer
    // opportunity; an offer accepted while current keeps its existing
    // stale-after-offer replay behavior.
    if (!session.isCurrent())
    {
      return;
    }

    _optPendingUndo.reset();
    ++_opportunityId;
    _optPendingUndo.emplace(
      std::in_place_type<TrackDetailCustomMetadataUndo>,
      TrackDetailCustomMetadataUndo{.key = std::move(key), .value = std::move(value), .session = std::move(session)});
    resetTimer();
    _changed.emit();
  }

  void TrackDetailUndoController::clearIfAffectsCustomMetadata(std::string_view const key,
                                                               std::vector<TrackId> const& trackIds)
  {
    auto const* pending = pendingCustomMetadataUndo();

    if (pending == nullptr || pending->key != key)
    {
      return;
    }

    auto const overlaps = std::ranges::any_of(trackIds,
                                              [pending](TrackId const trackId)
                                              { return std::ranges::contains(pending->session.targetIds(), trackId); });

    if (overlaps)
    {
      clear();
    }
  }

  void TrackDetailUndoController::presentCreditsClearedUndo(rt::CreditReplacement replacement,
                                                            uimodel::TrackAuthoringSession session)
  {
    // Same retirement rule as the custom-metadata offer: an invalidated
    // session cannot replay and must not displace a newer opportunity.
    if (!session.isCurrent())
    {
      return;
    }

    _optPendingUndo.reset();
    ++_opportunityId;
    _optPendingUndo.emplace(
      std::in_place_type<TrackDetailCreditsUndo>,
      TrackDetailCreditsUndo{.replacement = std::move(replacement), .session = std::move(session)});
    resetTimer();
    _changed.emit();
  }

  void TrackDetailUndoController::clearIfAffectsCredits(std::vector<TrackId> const& trackIds,
                                                        std::uint64_t const mutationRevision,
                                                        std::bitset<library::kCreditKindCount> kinds)
  {
    // A delayed completion must not invalidate a clear offered after that mutation committed.
    if (auto const* pending = pendingCreditsUndo();
        pending != nullptr && (pending->replacement.kinds & kinds).any() &&
        pending->session.boundRevision() <= mutationRevision &&
        std::ranges::any_of(trackIds,
                            [pending](TrackId trackId)
                            { return std::ranges::contains(pending->session.targetIds(), trackId); }))
    {
      clear();
    }
  }

  void TrackDetailUndoController::clear()
  {
    if (!_optPendingUndo)
    {
      return;
    }

    _optPendingUndo.reset();
    ++_opportunityId;
    disconnectTimer();
    _changed.emit();
  }

  async::Task<Result<>> TrackDetailUndoController::undoAsync()
  {
    if (!_optPendingUndo)
    {
      co_return Result<>{};
    }

    auto submission = std::visit(
      [](auto& pending)
      {
        auto patch = rt::MetadataPatch{};

        if constexpr (std::is_same_v<std::decay_t<decltype(pending)>, TrackDetailCustomMetadataUndo>)
        {
          patch.customUpdates[pending.key] = pending.value;
        }
        else
        {
          patch.optCredits = pending.replacement;
        }

        return pending.session.submitMetadataAsync(std::move(patch));
      },
      *_optPendingUndo);
    // A completion from an old replay cannot dismiss a replacement opportunity.
    auto clearPending = _presentationCallbacks.guard(
      [this, id = _opportunityId]
      {
        if (_opportunityId == id)
        {
          clear();
        }
      });

    auto const replyRes = co_await std::move(submission);

    if (!replyRes)
    {
      APP_LOG_ERROR("Metadata undo failed: {}", replyRes.error().message);
      auto error = replyRes.error();
      clearPending();
      co_return std::unexpected{std::move(error)};
    }

    auto res = Result<>{};

    switch (replyRes->status)
    {
      case rt::AuthoringStatus::Applied:
      case rt::AuthoringStatus::NoOp: break;
      case rt::AuthoringStatus::Busy: co_return makeError(Error::Code::ResourceBusy, "Metadata undo is currently busy");
      case rt::AuthoringStatus::Stale:
        res = makeError(Error::Code::InvalidState, "Library changed before metadata undo could be applied");
        break;
      case rt::AuthoringStatus::Unavailable:
        res = makeError(Error::Code::InvalidState, "Metadata undo is currently unavailable");
        break;
    }

    if (!res)
    {
      APP_LOG_ERROR("Metadata undo failed: {}", res.error().message);
    }

    clearPending();
    co_return res;
  }

  sigc::signal<void()>& TrackDetailUndoController::signalChanged()
  {
    return _changed;
  }

  void TrackDetailUndoController::resetTimer()
  {
    disconnectTimer();

    auto expire = _presentationCallbacks.guard(
      [this, id = _opportunityId]
      {
        if (_opportunityId == id)
        {
          clear();
        }
      });
    auto timeoutCallback = sigc::slot<bool()>{[expire = std::move(expire)] mutable
                                              {
                                                expire();
                                                return false;
                                              }};

    if (_timeoutScheduler)
    {
      _timerConn = _timeoutScheduler(kUndoTimeout, std::move(timeoutCallback));
      return;
    }

    _timerConn = Glib::signal_timeout().connect(std::move(timeoutCallback), kUndoTimeout.count());
  }

  void TrackDetailUndoController::disconnectTimer()
  {
    if (_timerConn)
    {
      _timerConn.disconnect();
    }
  }
} // namespace ao::gtk::layout
