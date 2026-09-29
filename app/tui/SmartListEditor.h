// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "CompletionPopup.h"
#include "MouseBindings.h"
#include "TextFieldModel.h"
#include <ao/CoreIds.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/completion/CompletionResult.h>
#include <ao/uimodel/library/list/SmartListEditing.h>

#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/box.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ftxui
{
  class Node;
  using Element = std::shared_ptr<Node>;
} // namespace ftxui

namespace ao::tui
{
  /// Whether the editor drafts a new List or rewrites an existing one.
  enum class ListEditorMode : std::uint8_t
  {
    New,
    Edit,
  };

  /// What the editor is currently allowed to do about writing.
  enum class ListEditorStatus : std::uint8_t
  {
    /// A valid draft can be submitted; a prior failure's diagnostic may still show.
    Ready,
    /// A submission is in flight; repeated submit and close are consumed.
    Submitting,
  };

  /// What the editor asked its owner for, taken exactly once.
  enum class ListEditorRequest : std::uint8_t
  {
    None,
    Close,
    Submit,
    /// A field edit changed what the preview must recompute.
    PreviewChanged,
  };

  /**
   * @brief The centered modal Saved-List editor for one drafted definition.
   *
   * The editor owns only field text and input routing. Every decision about
   * what the user may see or submit arrives through
   * uimodel::SmartListEditorViewState, which the owning controller recomputes
   * from a live track source.
   */
  class SmartListEditor final
  {
  public:
    /// Supplies expression completion candidates for the current text and cursor.
    using CompletionProvider = std::function<std::optional<rt::CompletionResult>(std::string_view, std::size_t)>;

    /// @p baseline carries the parent and edited ids plus the values each field starts from;
    /// @p parentExpression is the inherited expression of that parent, empty at the library root.
    SmartListEditor(i18n::MessageCatalog textCatalog,
                    ListEditorMode mode,
                    rt::ListDraft baseline,
                    std::string parentExpression,
                    CompletionProvider completionProvider = {});

    ListEditorMode mode() const noexcept { return _mode; }
    ListId parentListId() const noexcept { return _baseline.parentId; }
    ListId editListId() const noexcept { return _baseline.listId; }
    /// Whether any field left its baseline value.
    bool isDirty() const noexcept;
    /// Whether the last applied preview admits the current draft; the owner
    /// re-applies a fresh preview before trusting it for a submission.
    bool canSubmit() const noexcept;
    bool hasCompletion() const noexcept { return _optActiveCompletion.has_value(); }

    ListEditorStatus status() const noexcept { return _status; }
    std::string const& diagnostic() const noexcept { return _diagnostic; }
    /// Moves to @p status, replacing any diagnostic the previous status displayed.
    void setStatus(ListEditorStatus status, std::string diagnostic = {});
    bool isConfirmingDiscard() const noexcept { return _confirmingDiscard; }

    /// Consumes the pending owner request; a second call reports None.
    ListEditorRequest takeRequest() noexcept;

    /// The definition the current field values describe.
    rt::ListDraft draft() const;

    uimodel::SmartListEditorViewState const& viewState() const noexcept { return _viewState; }
    std::vector<std::string> const& previewTracks() const noexcept { return _previewTracks; }
    /// Replaces everything the controller recomputed about the draft's matches.
    void applyPreview(uimodel::SmartListEditorViewState viewState, std::vector<std::string> previewTracks);

    /// Consumes @p event; an active editor answers for every key the terminal delivers.
    bool tryHandleEvent(ftxui::Event const& event);
    ftxui::Element render() const;
    ftxui::Element renderModal(std::int32_t terminalColumns, std::int32_t terminalRows) const;

  private:
    /// One editable definition field.
    struct FieldRow final
    {
      i18n::MessageId labelId{};
      TextFieldModel input{};
      std::string baselineText{};
      /// Only the expression row completes; the others pass their edits through.
      bool completes = false;
      /// Muted guidance shown while the field is empty; Count shows none.
      i18n::MessageId placeholderId = i18n::MessageId::Count;
    };

    void moveField(std::int32_t delta);
    FieldRow& focusedField();
    FieldRow const& focusedField() const;
    /// The Inherited and Effective rows, shown only under a filtered parent,
    /// each held to one row of @p bodyColumns columns.
    ftxui::Element renderInheritedRows(std::int32_t bodyColumns) const;
    void noteFieldEdited();
    void maybeTriggerCompletion(bool explicitRequest);
    bool tryHandleCompletionEvent(ftxui::Event const& event);
    /// Replaces the focused field's completion range with the selected candidate.
    void acceptSelectedCompletion();
    bool tryHandleDiscardConfirmation(ftxui::Event const& event);
    void closeCompletion();
    void handleFieldEvent(ftxui::Event const& event);
    void handleMouse(ftxui::Mouse const& mouse);
    std::int32_t labelColumns() const;

    ftxui::Element renderFieldRow(std::size_t index) const;
    /// Renders the frame whose body fits @p bodyRows rows and whose wrapped
    /// footer text fits @p bodyColumns columns.
    ftxui::Element renderFrame(std::int32_t bodyRows, std::int32_t bodyColumns) const;
    /// The body shows every fixed row and as many preview tracks as @p rowBudget
    /// leaves, each single-row value held to @p bodyColumns columns.
    ftxui::Element renderBody(std::int32_t rowBudget, std::int32_t bodyColumns) const;
    ftxui::Element renderFooter(std::int32_t bodyColumns) const;
    /// The rows @ref renderFooter lays out at @p bodyColumns.
    std::int32_t footerRows(std::int32_t bodyColumns) const;
    /// The footer's wrapped notice: the discard prompt or the last diagnostic, empty when neither shows.
    std::string footerNotice() const;

    i18n::MessageCatalog _textCatalog;
    rt::ListDraft _baseline;
    ListEditorMode _mode = ListEditorMode::New;
    std::string _parentExpression{};
    std::vector<FieldRow> _fields{};
    std::size_t _focusedField = 0;
    CompletionProvider _completionProvider;
    std::optional<rt::CompletionResult> _optActiveCompletion{};
    CompletionPopupSelection _completionSelection{};
    uimodel::SmartListEditorViewState _viewState{};
    std::vector<std::string> _previewTracks{};
    ListEditorStatus _status = ListEditorStatus::Ready;
    ListEditorRequest _request = ListEditorRequest::None;
    std::string _diagnostic{};
    bool _confirmingDiscard = false;
    mutable bool _mouseReady = false;
    mutable MouseBindings _mouseBindings;
    mutable std::vector<ftxui::Box> _rowBoxes;
    mutable std::vector<ftxui::Box> _inputBoxes;
    mutable std::vector<ftxui::Box> _candidateBoxes;
  };
} // namespace ao::tui
