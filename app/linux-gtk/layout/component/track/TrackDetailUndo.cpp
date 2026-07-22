// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TrackDetailUndo.h"

#include <ao/CoreIds.h>
#include <ao/Error.h>
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

  Result<> TrackDetailUndoController::undo()
  {
    if (!_optPendingCustomMetadataUndo)
    {
      return {};
    }

    auto& pendingUndo = *_optPendingCustomMetadataUndo;

    auto patch = rt::MetadataPatch{};
    patch.customUpdates[pendingUndo.key] = pendingUndo.value;

    auto const replyResult = pendingUndo.sessionPtr->submitMetadata(patch);

    if (!replyResult)
    {
      APP_LOG_ERROR("Metadata undo failed: {}", replyResult.error().message);
      auto error = replyResult.error();
      clear();
      return std::unexpected{std::move(error)};
    }

    auto result = Result<>{};

    switch (replyResult->status)
    {
      case rt::TrackAuthoringStatus::Applied:
      case rt::TrackAuthoringStatus::NoOp: break;
      case rt::TrackAuthoringStatus::Stale:
        result = makeError(Error::Code::InvalidState, "Library changed before metadata undo could be applied");
        break;
      case rt::TrackAuthoringStatus::Missing:
        result = makeError(Error::Code::NotFound, "One or more tracks for metadata undo no longer exist");
        break;
      case rt::TrackAuthoringStatus::Unavailable:
        result = makeError(Error::Code::InvalidState, "Metadata undo is currently unavailable");
        break;
    }

    if (!result)
    {
      APP_LOG_ERROR("Metadata undo failed: {}", result.error().message);
    }

    clear();
    return result;
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
