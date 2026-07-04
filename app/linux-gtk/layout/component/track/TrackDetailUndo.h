// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>

#include <sigc++/connection.h>
#include <sigc++/functors/slot.h>
#include <sigc++/signal.h>

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ao::rt
{
  class LibraryWriter;
}

namespace ao::gtk::layout
{
  using TrackDetailUndoTimeoutScheduler =
    std::function<sigc::connection(std::chrono::milliseconds, sigc::slot<bool()>)>;

  struct TrackDetailCustomMetadataUndo final
  {
    std::string key;
    std::vector<TrackId> trackIds;
    std::string value;
  };

  class TrackDetailUndoController final
  {
  public:
    explicit TrackDetailUndoController(rt::LibraryWriter& writer,
                                       TrackDetailUndoTimeoutScheduler timeoutScheduler = {});
    ~TrackDetailUndoController();

    TrackDetailUndoController(TrackDetailUndoController const&) = delete;
    TrackDetailUndoController& operator=(TrackDetailUndoController const&) = delete;
    TrackDetailUndoController(TrackDetailUndoController&&) = delete;
    TrackDetailUndoController& operator=(TrackDetailUndoController&&) = delete;

    std::optional<TrackDetailCustomMetadataUndo> const& pendingCustomMetadataUndo() const;

    void showCustomMetadataDeleted(std::string key, std::vector<TrackId> trackIds, std::string value);
    void clearIfAffectsCustomMetadata(std::string_view key, std::vector<TrackId> const& trackIds);
    void clear();
    void undo();

    sigc::signal<void()>& signalChanged();

  private:
    void resetTimer();
    void disconnectTimer();

    rt::LibraryWriter& _writer;
    TrackDetailUndoTimeoutScheduler _timeoutScheduler;
    std::optional<TrackDetailCustomMetadataUndo> _optPendingCustomMetadataUndo;
    sigc::signal<void()> _changed;
    sigc::connection _timerConn;
  };
} // namespace ao::gtk::layout
