// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/uimodel/library/property/TrackAuthoringSession.h>

#include <sigc++/connection.h>
#include <sigc++/functors/slot.h>
#include <sigc++/signal.h>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ao::gtk::layout
{
  using TrackDetailUndoTimeoutScheduler =
    std::function<sigc::connection(std::chrono::milliseconds, sigc::slot<bool()>)>;

  struct TrackDetailCustomMetadataUndo final
  {
    std::string key;
    std::string value;
    std::unique_ptr<uimodel::TrackAuthoringSession> sessionPtr;
  };

  class TrackDetailUndoController final
  {
  public:
    explicit TrackDetailUndoController(TrackDetailUndoTimeoutScheduler timeoutScheduler = {});
    ~TrackDetailUndoController();

    TrackDetailUndoController(TrackDetailUndoController const&) = delete;
    TrackDetailUndoController& operator=(TrackDetailUndoController const&) = delete;
    TrackDetailUndoController(TrackDetailUndoController&&) = delete;
    TrackDetailUndoController& operator=(TrackDetailUndoController&&) = delete;

    std::optional<TrackDetailCustomMetadataUndo> const& pendingCustomMetadataUndo() const;

    void presentCustomMetadataDeletedUndo(std::string key,
                                          std::string value,
                                          std::unique_ptr<uimodel::TrackAuthoringSession> sessionPtr);
    void clearIfAffectsCustomMetadata(std::string_view key, std::vector<TrackId> const& trackIds);
    void clear();
    Result<> undo();

    sigc::signal<void()>& signalChanged();

  private:
    void resetTimer();
    void disconnectTimer();

    TrackDetailUndoTimeoutScheduler _timeoutScheduler;
    std::optional<TrackDetailCustomMetadataUndo> _optPendingCustomMetadataUndo;
    sigc::signal<void()> _changed;
    sigc::connection _timerConn;
  };
} // namespace ao::gtk::layout
