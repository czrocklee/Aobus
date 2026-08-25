// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TrackDetailUndo.h"

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Task.h>
#include <ao/rt/Log.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/uimodel/library/property/TrackAuthoringSession.h>

#include <glibmm/main.h>
#include <sigc++/functors/slot.h>
#include <sigc++/signal.h>

#include <algorithm>
#include <chrono>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
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

  std::optional<TrackDetailCustomMetadataUndo> const& TrackDetailUndoController::pendingCustomMetadataUndo() const
  {
    return _optPendingCustomMetadataUndo;
  }

  void TrackDetailUndoController::presentCustomMetadataDeletedUndo(
    std::string key,
    std::string value,
    std::unique_ptr<uimodel::TrackAuthoringSession> sessionPtr)
  {
    _optPendingCustomMetadataUndo = TrackDetailCustomMetadataUndo{
      .key = std::move(key), .value = std::move(value), .sessionPtr = std::move(sessionPtr)};
    resetTimer();
    _changed.emit();
  }

  void TrackDetailUndoController::clearIfAffectsCustomMetadata(std::string_view const key,
                                                               std::vector<TrackId> const& trackIds)
  {
    if (!_optPendingCustomMetadataUndo || _optPendingCustomMetadataUndo->key != key)
    {
      return;
    }

    auto const overlaps = std::ranges::any_of(
      trackIds,
      [this](TrackId const trackId)
      { return std::ranges::contains(_optPendingCustomMetadataUndo->sessionPtr->targetIds(), trackId); });

    if (overlaps)
    {
      clear();
    }
  }

  void TrackDetailUndoController::clear()
  {
    if (!_optPendingCustomMetadataUndo)
    {
      return;
    }

    _optPendingCustomMetadataUndo.reset();
    disconnectTimer();
    _changed.emit();
  }

  async::Task<Result<>> TrackDetailUndoController::undo()
  {
    if (!_optPendingCustomMetadataUndo)
    {
      co_return Result<>{};
    }

    auto patch = rt::MetadataPatch{};
    patch.customUpdates[_optPendingCustomMetadataUndo->key] = _optPendingCustomMetadataUndo->value;
    auto submission = _optPendingCustomMetadataUndo->sessionPtr->submitMetadata(std::move(patch));
    auto clearPending = _presentationCallbacks.guard([this] { clear(); });

    auto const replyRes = co_await std::move(submission);

    if (!replyRes)
    {
      APP_LOG_ERROR("Metadata undo failed: {}", replyRes.error().message);
      auto error = replyRes.error();
      clearPending();
      co_return std::unexpected{std::move(error)};
    }

    auto result = Result<>{};

    switch (replyRes->status)
    {
      case rt::AuthoringStatus::Applied:
      case rt::AuthoringStatus::NoOp: break;
      case rt::AuthoringStatus::Busy: co_return makeError(Error::Code::ResourceBusy, "Metadata undo is currently busy");
      case rt::AuthoringStatus::Stale:
        result = makeError(Error::Code::InvalidState, "Library changed before metadata undo could be applied");
        break;
      case rt::AuthoringStatus::Unavailable:
        result = makeError(Error::Code::InvalidState, "Metadata undo is currently unavailable");
        break;
    }

    if (!result)
    {
      APP_LOG_ERROR("Metadata undo failed: {}", result.error().message);
    }

    clearPending();
    co_return result;
  }

  sigc::signal<void()>& TrackDetailUndoController::signalChanged()
  {
    return _changed;
  }

  void TrackDetailUndoController::resetTimer()
  {
    disconnectTimer();

    auto timeoutCallback = sigc::slot<bool()>{[this]
                                              {
                                                clear();
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
