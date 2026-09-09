// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "TrackPropertiesEditor.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Task.h>
#include <ao/uimodel/library/track/TrackAuthoringSessions.h>

#include <ftxui/component/event.hpp>

#include <functional>
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
  class CompletionService;
  class Library;
  class NotificationService;
  class TextOrderingPolicy;
}

namespace ao::tui
{
  /**
   * @brief Owns the terminal shell's single track metadata editor.
   *
   * Opening, submission bookkeeping, invalidation, and retirement all run on
   * the callback executor, the same serialized lane as TUI event dispatch.
   *
   * One open request makes one coherent preparation attempt: one authoring
   * session bound to the whole captured target vector, and one snapshot whose
   * revision must match that binding. Nothing partial is ever installed, so a
   * refused open leaves marks, focus, and input exactly as they were.
   *
   * A submitted write outlives the editor that started it, which is what lets
   * exit wait for it: @ref hasPendingSubmission stays true until the write
   * settles, whether or not the editor is still on screen.
   */
  class TrackEditController final
  {
  public:
    struct Outputs final
    {
      /// Asks the shell to redraw after asynchronous state changed.
      std::function<void()> requestRefresh;
      /// Reports one submitted write settling, for the graceful-exit gate.
      std::function<void()> notifySubmittedWriteSettled{};
    };

    TrackEditController(async::Runtime& runtime,
                        rt::Library& library,
                        rt::NotificationService& notifications,
                        i18n::MessageCatalog const& textCatalog,
                        Outputs outputs,
                        rt::CompletionService& completionService,
                        rt::TextOrderingPolicy const* textOrderingPolicy);

    TrackEditController(TrackEditController const&) = delete;
    TrackEditController& operator=(TrackEditController const&) = delete;
    TrackEditController(TrackEditController&&) = delete;
    TrackEditController& operator=(TrackEditController&&) = delete;

    ~TrackEditController();

    /**
     * @brief Opens an editor over @p targetIds, reporting whether one was installed.
     *
     * The ids are captured as given; later filtering, reordering, or mark
     * changes cannot shrink or grow what the editor writes. A refusal reports
     * itself through the notification feed and changes nothing else.
     */
    bool tryOpen(std::vector<TrackId> targetIds);

    /// The editor to render and route input to, or null when none is tryOpen.
    TrackPropertiesEditor const* activeEditor() const noexcept;
    bool isActive() const noexcept { return activeEditor() != nullptr; }
    /// Whether a submitted write is still settling, editor tryOpen or not.
    bool hasPendingSubmission() const noexcept;

    /// Routes @p event to the open editor; reports false only when none is open.
    bool tryHandleEvent(ftxui::Event const& event);

    /// Closes any open editor without waiting for a submitted write to settle.
    void retire();

  private:
    struct State;

    /**
     * @brief Awaits @p submission and settles it on the callback executor.
     *
     * The task is created by the caller from a session it has just found
     * current, so this coroutine never reads the session again and can outlive
     * both it and the editor that started the write.
     */
    static async::Task<void> runSubmitAsync(std::shared_ptr<State> statePtr,
                                            async::Task<Result<uimodel::TrackPropertiesSubmitResult>> submission);

    /// Acts on whatever the editor asked for during the last event.
    void serviceRequest();
    void submit();
    void reload();

    std::shared_ptr<State> _statePtr;
  };
} // namespace ao::tui
