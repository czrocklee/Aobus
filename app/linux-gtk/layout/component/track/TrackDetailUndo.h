// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "common/MainContextCallbackScope.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Task.h>
#include <ao/library/Credits.h>
#include <ao/rt/TrackMutation.h>
#include <ao/uimodel/library/track/TrackAuthoringSessions.h>

#include <sigc++/connection.h>
#include <sigc++/functors/slot.h>
#include <sigc++/signal.h>

#include <bitset>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace ao::gtk::layout
{
  using TrackDetailUndoTimeoutScheduler =
    std::function<sigc::connection(std::chrono::milliseconds, sigc::slot<bool()>)>;

  struct TrackDetailCustomMetadataUndo final
  {
    std::string key;
    std::string value;
    uimodel::TrackAuthoringSession session;
  };

  struct TrackDetailCreditsUndo final
  {
    rt::CreditReplacement replacement;
    uimodel::TrackAuthoringSession session;
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

    TrackDetailCustomMetadataUndo const* pendingCustomMetadataUndo() const;
    TrackDetailCreditsUndo const* pendingCreditsUndo() const;

    void presentCustomMetadataDeletedUndo(std::string key, std::string value, uimodel::TrackAuthoringSession session);
    void clearIfAffectsCustomMetadata(std::string_view key, std::vector<TrackId> const& trackIds);
    void presentCreditsClearedUndo(rt::CreditReplacement replacement, uimodel::TrackAuthoringSession session);
    void clearIfAffectsCredits(std::vector<TrackId> const& trackIds,
                               std::uint64_t mutationRevision,
                               std::bitset<library::kCreditKindCount> kinds);
    void clear();
    async::Task<Result<>> undoAsync();

    sigc::signal<void()>& signalChanged();

  private:
    void resetTimer();
    void disconnectTimer();

    TrackDetailUndoTimeoutScheduler _timeoutScheduler;
    std::optional<std::variant<TrackDetailCustomMetadataUndo, TrackDetailCreditsUndo>> _optPendingUndo;
    std::uint64_t _opportunityId = 0;
    sigc::signal<void()> _changed;
    sigc::connection _timerConn;
    MainContextCallbackScope _presentationCallbacks;
  };
} // namespace ao::gtk::layout
