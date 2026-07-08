// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TrackDetailUndo.h"

#include <ao/CoreIds.h>
#include <ao/rt/Log.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/library/LibraryWriter.h>

#include <glibmm/main.h>
#include <sigc++/functors/slot.h>
#include <sigc++/signal.h>

#include <algorithm>
#include <chrono>
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

  TrackDetailUndoController::TrackDetailUndoController(rt::LibraryWriter& writer,
                                                       TrackDetailUndoTimeoutScheduler timeoutScheduler)
    : _writer{writer}, _timeoutScheduler{std::move(timeoutScheduler)}
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

  void TrackDetailUndoController::presentCustomMetadataDeletedUndo(std::string key,
                                                                   std::vector<TrackId> trackIds,
                                                                   std::string value)
  {
    _optPendingCustomMetadataUndo =
      TrackDetailCustomMetadataUndo{.key = std::move(key), .trackIds = std::move(trackIds), .value = std::move(value)};
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

    auto const overlaps =
      std::ranges::any_of(trackIds,
                          [this](TrackId const trackId)
                          { return std::ranges::contains(_optPendingCustomMetadataUndo->trackIds, trackId); });

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

  void TrackDetailUndoController::undo()
  {
    if (!_optPendingCustomMetadataUndo)
    {
      return;
    }

    auto const pendingUndo = *_optPendingCustomMetadataUndo;

    auto patch = rt::MetadataPatch{};
    patch.customUpdates[pendingUndo.key] = pendingUndo.value;

    if (auto const replyResult = _writer.updateMetadata(pendingUndo.trackIds, patch); !replyResult)
    {
      APP_LOG_ERROR("Metadata undo failed: {}", replyResult.error().message);
      return;
    }

    clear();
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
