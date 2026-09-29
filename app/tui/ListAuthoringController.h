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
   * @brief The delete flow's confirmation surface: preview facts plus the user's pending answer.
   */
  struct ListDeleteConfirmation final
  {
    ListId listId = kInvalidListId;
    bool includeDescendants = false;
    /// Localized confirmation heading and body, built from the settled preview.
    std::string title{};
    std::string question{};
    /// The tag-removal offer; empty when the preview reported no writable membership tag.
    std::string tagImpactQuestion{};
    /// The shared warning about other Lists referencing that tag; empty when none do.
    std::string tagReferencesWarning{};
    /// Whether the user asked to also remove the writable tag from its tracks.
    bool removeWritableTag = false;
    /// A deletion was admitted and is settling; every further input is consumed.
    bool deleting = false;
  };

  /**
   * @brief Owns the terminal shell's single Saved-List authoring flow.
   *
   * Opening, preview recomputation, submission bookkeeping, and retirement all
   * run on the callback executor, the same serialized lane as TUI event
   * dispatch. The controller drafts one List definition at a time: `new` and
   * `edit` install a modal editor over a coherent snapshot read, while
   * `delete` first previews the deletion and then asks one bounded question.
   *
   * A submitted save or deletion outlives the surface that started it, which
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
      /**
       * @brief Ends workspace gestures and transient input.
       *
       * The delete confirmation is the one surface that appears without a
       * keypress, so the shell interactions it interrupts are retired here
       * rather than by the event that opened it.
       */
      std::function<void()> cancelTransientInteractions{};
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

    /**
     * @brief Starts the deletion flow for @p targetListId with a runtime preview.
     *
     * A virtual target, and a List that is not a real saved List, are refused
     * through the notification feed. The confirmation surface appears when the
     * preview settles.
     */
    bool tryDelete(ListId targetListId);

    /// The editor to render and route input to, or null when none is installed.
    SmartListEditor const* activeEditor() const noexcept;
    /// The delete flow's confirmation, or null when no deletion is being confirmed.
    ListDeleteConfirmation const* activeDeleteConfirmation() const noexcept;
    /// The active surface's modal element, or null when nothing is on screen.
    ftxui::Element activeModal(std::int32_t terminalColumns, std::int32_t terminalRows) const;
    /// Whether any authoring surface is on screen and owns input.
    bool isActive() const noexcept;
    /// Whether a submitted save or deletion is still settling, surface visible or not.
    bool hasPendingSubmission() const noexcept;
    /// Whether any authoring operation is in flight, including a deletion preview.
    bool isBusy() const noexcept;

    /// Routes @p event to the open surface; reports false when none is open.
    bool tryHandleEvent(ftxui::Event const& event);

    /// Closes any open surface without waiting for a submitted write to settle.
    void retire();

  private:
    struct State;

    static async::Task<void> runSaveAsync(std::shared_ptr<State> statePtr, async::Task<Result<ListId>> submission);
    static async::Task<void> runDeletePreviewAsync(std::shared_ptr<State> statePtr,
                                                   bool includeDescendants,
                                                   async::Task<Result<rt::DeleteListSubtreeReply>> preview);
    static async::Task<void> runDeleteAsync(std::shared_ptr<State> statePtr,
                                            async::Task<Result<rt::DeleteListSubtreeReply>> deletion);
    static async::Task<void> runPreviewDebounceAsync(async::Runtime* runtime,
                                                     std::shared_ptr<State> statePtr,
                                                     std::uint64_t generation,
                                                     std::stop_token stopToken);

    /// Acts on whatever the editor asked for during the last event.
    void serviceRequest();
    void submit();
    void schedulePreview();
    void cancelPreviewDebounce();
    void handleDeleteConfirmationEvent(ftxui::Event const& event);
    ftxui::Element renderDeleteConfirmation(ListDeleteConfirmation const& confirmation,
                                            std::int32_t terminalColumns,
                                            std::int32_t terminalRows) const;

    std::shared_ptr<State> _statePtr;
  };
} // namespace ao::tui
