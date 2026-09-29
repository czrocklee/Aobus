// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/rt/ViewIds.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace ao::async
{
  class Runtime;
}

namespace ao::i18n
{
  class MessageCatalog;
}

namespace ao::rt
{
  class Library;
  class NotificationService;
  class ViewService;
}

namespace ao::tui
{
  /// One manual List order intent the terminal shell can submit.
  enum class ListOrderCommand : std::uint8_t
  {
    MoveUp,
    MoveDown,
    MoveToTop,
    MoveToBottom,
    Reset,
  };

  /**
   * @brief Submits manual List order commands and reports their outcomes.
   *
   * Each command begins its own revision-bound authoring session, matching the
   * keyboard and menu paths of the other frontends: an applied command consumes
   * its session, so the next command always starts from a fresh binding.
   * Outcomes and blocking reasons are reported through the notification feed.
   */
  class ListOrderController final
  {
  public:
    ListOrderController(async::Runtime& runtime,
                        rt::Library& library,
                        rt::ViewService& views,
                        rt::NotificationService& notifications,
                        i18n::MessageCatalog const& textCatalog);

    ListOrderController(ListOrderController const&) = delete;
    ListOrderController& operator=(ListOrderController const&) = delete;
    ListOrderController(ListOrderController&&) = delete;
    ListOrderController& operator=(ListOrderController&&) = delete;

    ~ListOrderController() = default;

    /**
     * @brief Submits @p command for @p viewId over @p selectedTrackIds.
     *
     * Called on the callback executor, the same serialized lane as TUI event
     * dispatch; the submission reports its outcome later on that executor. A
     * command arriving while one submission is still in flight is dropped
     * rather than queued, so terminal key repeat cannot bind against a
     * revision the in-flight commit is already changing.
     */
    void apply(ListOrderCommand command, rt::ViewId viewId, std::vector<TrackId> selectedTrackIds);

  private:
    struct State;

    std::shared_ptr<State> _statePtr;
  };
} // namespace ao::tui
