// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/async/Task.h>
#include <ao/compat/MoveOnlyFunction.h>
#include <ao/uimodel/library/task/LibraryScanOutcome.h>

#include <cstdint>
#include <memory>
#include <stop_token>

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
  class LibraryJobs;
  class NotificationService;
}

namespace ao::tui
{
  /**
   * @brief One restartable library scan for the terminal shell.
   *
   * Start, cancel, retire, and completion bookkeeping run on the callback
   * executor, the same serialized lane as TUI event dispatch. @ref phase is
   * unsynchronized and is only safe to read on that lane. A stop request does
   * not start a replacement while cancellation is still settling.
   */
  class LibraryScanController final
  {
  public:
    enum class Phase : std::uint8_t
    {
      Idle,
      Running,
      Cancelling,
      Retired,
    };

    using ScanTask = compat::MoveOnlyFunction<async::Task<uimodel::LibraryScanOutcome>(std::stop_token)>;

    LibraryScanController(async::Runtime& runtime,
                          rt::NotificationService& notifications,
                          i18n::MessageCatalog const& catalog,
                          ScanTask scan);
    LibraryScanController(async::Runtime& runtime,
                          rt::LibraryJobs& jobs,
                          rt::NotificationService& notifications,
                          i18n::MessageCatalog const& catalog);

    LibraryScanController(LibraryScanController const&) = delete;
    LibraryScanController& operator=(LibraryScanController const&) = delete;
    LibraryScanController(LibraryScanController&&) = delete;
    LibraryScanController& operator=(LibraryScanController&&) = delete;

    ~LibraryScanController();

    void start();
    void cancel();
    void retire();
    Phase phase() const noexcept;

  private:
    struct State;

    static async::Task<void> runScan(std::shared_ptr<State> statePtr, std::stop_token stopToken);

    std::shared_ptr<State> _statePtr;
  };
} // namespace ao::tui
