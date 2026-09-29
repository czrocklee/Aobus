// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "SmartListEditor.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Task.h>
#include <ao/rt/ListMutation.h>
#include <ao/uimodel/library/presentation/ListPresentations.h>

#include <ftxui/component/event.hpp>
#include <ftxui/dom/node.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>

namespace ftxui
{
  class Node;
  using Element = std::shared_ptr<Node>;
} // namespace ftxui

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
  class TrackSourceCache;
  class ViewService;
  class WorkspaceService;
}

namespace ao::tui
{
  /**
   * @brief Owns the terminal shell's single Saved-List authoring flow.
   *
   * Opening, preview recomputation, submission bookkeeping, and retirement all
   * run on the callback executor, the same serialized lane as TUI event
   * dispatch. The controller drafts one List definition at a time: `new` and
   * `edit` install a modal editor over a coherent snapshot read.
   *
   * A submitted save outlives the surface that started it, which
   * is what lets exit wait for it: @ref hasPendingSubmission stays true until
   * the write settles, whether or not anything is still on screen.
   */
  class ListAuthoringController final
  {
  public:
    struct Outputs final
    {
      /// Asks the shell to redraw after asynchronous state changed.
      std::function<void()> requestRefresh;
      /// Reports one submitted write settling, for the graceful-exit gate.
      std::function<void()> notifySubmittedWriteSettled{};
      /// Shows a List the user just created, so the result of the save is on screen.
      std::function<void(ListId)> openCreatedList{};
    };

    ListAuthoringController(async::Runtime& runtime,
                            rt::Library& library,
                            rt::ViewService& views,
                            rt::TrackSourceCache& sources,
                            rt::WorkspaceService& workspace,
                            rt::CompletionService& completion,
                            rt::NotificationService& notifications,
                            uimodel::ListPresentations& listPresentations,
                            i18n::MessageCatalog const& textCatalog,
                            Outputs outputs);

    ListAuthoringController(ListAuthoringController const&) = delete;
    ListAuthoringController& operator=(ListAuthoringController const&) = delete;
    ListAuthoringController(ListAuthoringController&&) = delete;
    ListAuthoringController& operator=(ListAuthoringController&&) = delete;

    ~ListAuthoringController();

    /**
     * @brief Opens a new-List editor whose parent derives from @p targetListId.
     *
     * A virtual target parents at the library root. A refusal reports itself
     * through the notification feed and changes nothing else.
     */
    bool tryOpenNew(ListId targetListId);

    /// Opens an editor over the saved definition of @p targetListId, reporting whether one was installed.
    bool tryOpenEdit(ListId targetListId);

    /// The editor to render and route input to, or null when none is installed.
    SmartListEditor const* activeEditor() const noexcept;
    /// The active surface's modal element, or null when nothing is on screen.
    ftxui::Element activeModal(std::int32_t terminalColumns, std::int32_t terminalRows) const;
    /// Whether any authoring surface is on screen and owns input.
    bool isActive() const noexcept;
    /// Whether a submitted save is still settling, surface visible or not.
    bool hasPendingSubmission() const noexcept;

    /// Routes @p event to the open surface; reports false when none is open.
    bool tryHandleEvent(ftxui::Event const& event);

    /// Closes any open surface without waiting for a submitted write to settle.
    void retire();

  private:
    struct State;

    static async::Task<void> runSaveAsync(std::shared_ptr<State> statePtr, async::Task<Result<ListId>> submission);
    static async::Task<void> runPreviewDebounceAsync(async::Runtime* runtime,
                                                     std::shared_ptr<State> statePtr,
                                                     std::uint64_t generation,
                                                     std::stop_token stopToken);

    /// Acts on whatever the editor asked for during the last event.
    void serviceRequest();
    void submit();
    void schedulePreview();
    void cancelPreviewDebounce();

    std::shared_ptr<State> _statePtr;
  };
} // namespace ao::tui
