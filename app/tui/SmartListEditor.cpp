// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "SmartListEditor.h"

#include "CompletionPopup.h"
#include "MouseBindings.h"
#include "Style.h"
#include "TextCell.h"
#include "TextField.h"
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/completion/CompletionResult.h>
#include <ao/uimodel/library/list/SmartListEditing.h>

#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::tui
{
  namespace
  {
    using i18n::MessageId;

    /// Cap for the measured label column: the widest localized field label
    /// sets the budget, so a pathological translation cannot consume the
    /// input column while real labels stay untruncated.
    constexpr std::int32_t kMaximumLabelColumns = 24;
    /// Cells a field row spends outside its label: the focus marker and the gap before the value.
    constexpr std::int32_t kFocusMarkerColumns = 2;
    constexpr std::int32_t kLabelGapColumns = 1;
    /// Frame rows around the body: border, header, and the two separators.
    constexpr std::int32_t kFrameChromeRows = 5;
    /// Frame columns around wrapped text: border and panel padding on both sides.
    constexpr std::int32_t kFrameChromeColumns = 4;
    /// The completion popup's top and bottom border.
    constexpr std::int32_t kPopupBorderRows = 2;
    /// The Inherited and Effective expression rows.
    constexpr std::int32_t kInheritedRows = 2;

    /// Wrapped rows of @p value, one element each, so the caller can count exactly what it lays out.
    ftxui::Elements wrappedRows(std::string_view const value, std::int32_t const columns)
    {
      auto rows = ftxui::Elements{};

      for (auto& line : wrapCellText(value, std::max(1, columns)))
      {
        rows.push_back(ftxui::text(std::move(line)));
      }

      return rows;
    }

    /// Whether the sole candidate already spells the token it would replace,
    /// wherever that token sits in the expression, so offering it adds nothing.
    bool isAlreadyComplete(std::string_view const value, rt::CompletionResult const& result)
    {
      if (result.items.size() != 1 || result.replaceBegin > result.replaceEnd || result.replaceEnd > value.size())
      {
        return false;
      }

      return result.items.front().insertText ==
             value.substr(result.replaceBegin, result.replaceEnd - result.replaceBegin);
    }
  } // namespace

  SmartListEditor::SmartListEditor(i18n::MessageCatalog textCatalog,
                                   ListEditorMode const mode,
                                   rt::ListDraft baseline,
                                   std::string parentExpression,
                                   CompletionProvider completionProvider)
    : _textCatalog{std::move(textCatalog)}
    , _baseline{std::move(baseline)}
    , _mode{mode}
    , _parentExpression{std::move(parentExpression)}
    , _completionProvider{std::move(completionProvider)}
  {
    auto const makeField =
      [](MessageId const labelId, std::string value, bool const completes, MessageId const placeholderId)
    {
      auto field = FieldRow{
        .labelId = labelId, .input = TextFieldModel{value}, .completes = completes, .placeholderId = placeholderId};
      field.baselineText = std::move(value);
      return field;
    };

    _fields.push_back(makeField(MessageId::TuiListFieldName, _baseline.name, false, MessageId::Count));
    _fields.push_back(makeField(MessageId::TuiListFieldDescription, _baseline.description, false, MessageId::Count));
    _fields.push_back(makeField(
      MessageId::TuiListFieldExpression, _baseline.expression, true, MessageId::TuiListExpressionPlaceholder));
  }

  bool SmartListEditor::isDirty() const noexcept
  {
    return std::ranges::any_of(
      _fields, [](FieldRow const& field) { return field.input.value() != field.baselineText; });
  }

  bool SmartListEditor::canSubmit() const noexcept
  {
    // Name presence and expression validity are the same two gates the shared
    // view state applies, recomputed here so a fresh name edit takes effect
    // before the debounced preview lands.
    return !_fields.empty() && !_fields.front().input.empty() && _viewState.expressionValid;
  }

  void SmartListEditor::setStatus(ListEditorStatus const status, std::string diagnostic)
  {
    _status = status;
    _diagnostic = std::move(diagnostic);

    // A question or request about a draft is meaningless once the write it
    // guarded is already running.
    if (_status != ListEditorStatus::Ready)
    {
      closeCompletion();
      _confirmingDiscard = false;
      _request = ListEditorRequest::None;
    }
  }

  ListEditorRequest SmartListEditor::takeRequest() noexcept
  {
    return std::exchange(_request, ListEditorRequest::None);
  }

  rt::ListDraft SmartListEditor::draft() const
  {
    return uimodel::makeSmartListDraft(_baseline.parentId,
                                       _baseline.listId,
                                       _fields[0].input.value(),
                                       _fields[1].input.value(),
                                       _fields[2].input.value());
  }

  void SmartListEditor::applyPreview(uimodel::SmartListEditorViewState viewState,
                                     std::vector<std::string> previewTracks)
  {
    _viewState = std::move(viewState);
    _previewTracks = std::move(previewTracks);
  }

  bool SmartListEditor::tryHandleEvent(ftxui::Event const& event)
  {
    // A write in flight cannot be steered, cancelled, or escaped from, so the
    // surface stays visible and inert until its terminal result arrives.
    if (_status == ListEditorStatus::Submitting)
    {
      return true;
    }

    if (event.is_mouse())
    {
      auto mouseEvent = event;
      auto const& mouse = mouseEvent.mouse();

      if ((!isLeftPress(mouse) && mouseWheelDirection(mouse) == 0) || !std::exchange(_mouseReady, false))
      {
        return true;
      }

      handleMouse(mouse);
      return true;
    }

    _mouseReady = false;

    if (tryHandleDiscardConfirmation(event))
    {
      return true;
    }

    if (tryHandleCompletionEvent(event))
    {
      return true;
    }

    if (event == ftxui::Event::Escape)
    {
      // A draft nobody confirmed losing is worth one question; a clean editor
      // closes at once because there is nothing to lose.
      if (isDirty())
      {
        _confirmingDiscard = true;
      }
      else
      {
        _request = ListEditorRequest::Close;
      }

      return true;
    }

    if (event == ftxui::Event::CtrlS)
    {
      // The landed preview may still describe text from before the last
      // keystroke, so the editor does not judge validity here: the owner
      // recomputes the preview from this draft and decides.
      _request = ListEditorRequest::Submit;
      return true;
    }

    if (event == ftxui::Event::ArrowUp || event == ftxui::Event::ArrowDown)
    {
      moveField(event == ftxui::Event::ArrowUp ? -1 : 1);
      return true;
    }

    if (event == ftxui::Event::Return)
    {
      // With nothing to accept, Return walks the form, wrapping so the last
      // field can reach the first; the fields stay plain single-line inputs.
      closeCompletion();
      _focusedField = (_focusedField + 1) % _fields.size();
      return true;
    }

    handleFieldEvent(event);
    return true;
  }

  ftxui::Element SmartListEditor::render() const
  {
    return renderFrame(std::numeric_limits<std::int32_t>::max(), std::numeric_limits<std::int32_t>::max());
  }

  ftxui::Element SmartListEditor::renderFrame(std::int32_t const bodyRows, std::int32_t const bodyColumns) const
  {
    using namespace ftxui;

    auto const title = std::string{i18n::requiredText(
      _textCatalog,
      _mode == ListEditorMode::New ? MessageId::TuiListEditorNewTitle : MessageId::TuiListEditorEditTitle)};

    _mouseBindings.clear();
    _mouseReady = true;

    auto header = Elements{text(title) | bold};

    if (isDirty())
    {
      header.push_back(text(" *") | style::warning());
    }

    header.push_back(filler());
    header.push_back(_mouseBindings.bind(text(" × "), Event::Escape));

    return vbox({
             style::panelBody(hbox(std::move(header))),
             style::panelBody(separator()),
             style::panelBody(renderBody(bodyRows, bodyColumns)) | flex,
             style::panelBody(separator()),
             style::panelBody(renderFooter(bodyColumns)),
           }) |
           border;
  }

  ftxui::Element SmartListEditor::renderModal(std::int32_t const terminalColumns, std::int32_t const terminalRows) const
  {
    using namespace ftxui;

    // A terminal narrower or shorter than the preferred box gets the whole of
    // itself instead, which is what makes 80x24 a full-surface presentation.
    auto const modalCols = std::min(terminalColumns, std::clamp(terminalColumns - 4, 80, 100));
    auto const modalRows = std::min(terminalRows, std::clamp(terminalRows - 2, 24, 30));

    // The body gets what the frame and the footer leave, so the preview sample
    // shrinks on a short terminal instead of pushing rows out of sight.
    auto const bodyColumns = std::max(1, modalCols - kFrameChromeColumns);
    auto const bodyRows = std::max(0, modalRows - kFrameChromeRows - footerRows(bodyColumns));
    auto boxPtr =
      renderFrame(bodyRows, bodyColumns) | size(WIDTH, EQUAL, modalCols) | size(HEIGHT, EQUAL, modalRows) | clear_under;

    if (modalCols >= terminalColumns && modalRows >= terminalRows)
    {
      return boxPtr;
    }

    return vbox({
      filler(),
      hbox({
        filler(),
        std::move(boxPtr),
        filler(),
      }),
      filler(),
    });
  }

  void SmartListEditor::moveField(std::int32_t const delta)
  {
    if (_fields.empty())
    {
      return;
    }

    closeCompletion();
    auto const last = static_cast<std::int32_t>(_fields.size()) - 1;
    auto const target = std::clamp(static_cast<std::int32_t>(_focusedField) + delta, 0, last);
    _focusedField = static_cast<std::size_t>(target);
  }

  SmartListEditor::FieldRow& SmartListEditor::focusedField()
  {
    return _fields[_focusedField];
  }

  SmartListEditor::FieldRow const& SmartListEditor::focusedField() const
  {
    return _fields[_focusedField];
  }

  void SmartListEditor::noteFieldEdited()
  {
    if (_status == ListEditorStatus::Ready)
    {
      _diagnostic.clear();
    }

    _request = ListEditorRequest::PreviewChanged;
  }

  void SmartListEditor::maybeTriggerCompletion(bool const explicitRequest)
  {
    auto const& field = focusedField();

    if (!field.completes || !_completionProvider)
    {
      closeCompletion();
      return;
    }

    if (!explicitRequest && field.input.empty())
    {
      closeCompletion();
      return;
    }

    auto optResult = _completionProvider(field.input.value(), field.input.cursor());

    if (!optResult || optResult->items.empty())
    {
      closeCompletion();
      return;
    }

    if (!explicitRequest && isAlreadyComplete(field.input.value(), *optResult))
    {
      closeCompletion();
      return;
    }

    _optActiveCompletion = std::move(optResult);
    _completionSelection.reset();
  }

  void SmartListEditor::acceptSelectedCompletion()
  {
    // The active result is taken by value: accepting is one event, and a
    // local copy keeps the guarded access honest even if the popup is
    // dismissed while the replacement runs.
    auto const optCompletion = _optActiveCompletion;

    if (!optCompletion || _completionSelection.selectedCandidate() >= optCompletion->items.size())
    {
      return;
    }

    auto& field = focusedField();

    if (auto const& item = optCompletion->items[_completionSelection.selectedCandidate()];
        field.input.tryReplaceRange(optCompletion->replaceBegin, optCompletion->replaceEnd, item.insertText))
    {
      noteFieldEdited();
    }
  }

  bool SmartListEditor::tryHandleCompletionEvent(ftxui::Event const& event)
  {
    if (!_optActiveCompletion)
    {
      return false;
    }

    if (_completionSelection.tryNavigate(event, _optActiveCompletion->items.size()))
    {
      return true;
    }

    if (event == ftxui::Event::Return)
    {
      acceptSelectedCompletion();
      closeCompletion();
      return true;
    }

    if (event == ftxui::Event::Escape)
    {
      closeCompletion();
      return true;
    }

    if (event == ftxui::Event::ArrowLeft || event == ftxui::Event::ArrowRight || event == ftxui::Event::Home ||
        event == ftxui::Event::End || event == ftxui::Event::CtrlA || event == ftxui::Event::CtrlE ||
        event == ftxui::Event::ArrowLeftCtrl || event == ftxui::Event::ArrowRightCtrl)
    {
      closeCompletion();
      return false;
    }

    if (event == ftxui::Event::CtrlS || event == ftxui::Event::Tab || event == ftxui::Event::TabReverse)
    {
      closeCompletion();
      return false;
    }

    return false;
  }

  bool SmartListEditor::tryHandleDiscardConfirmation(ftxui::Event const& event)
  {
    // A confirmation owns the keyboard until it is answered, so a stray key
    // cannot both dismiss the question and act on the form behind it.
    if (!_confirmingDiscard)
    {
      return false;
    }

    if (event == ftxui::Event::Return)
    {
      _request = ListEditorRequest::Close;
    }
    else if (event != ftxui::Event::Escape)
    {
      return true;
    }

    _confirmingDiscard = false;
    return true;
  }

  void SmartListEditor::closeCompletion()
  {
    _candidateBoxes.clear();
    _optActiveCompletion.reset();
    _completionSelection.reset();
  }

  void SmartListEditor::handleFieldEvent(ftxui::Event const& event)
  {
    auto& field = focusedField();

    if (event == ftxui::Event::CtrlN)
    {
      maybeTriggerCompletion(true);
      return;
    }

    if (auto const previousCursor = field.input.cursor(); field.input.tryApplyEvent(event))
    {
      noteFieldEdited();
      maybeTriggerCompletion(false);
    }
    else if (field.input.cursor() != previousCursor)
    {
      closeCompletion();
    }
  }

  void SmartListEditor::handleMouse(ftxui::Mouse const& mouse)
  {
    if (auto const optEvent = _mouseBindings.eventAt(mouse); optEvent)
    {
      _mouseBindings.clear();
      tryHandleEvent(*optEvent);
      return;
    }

    if (_confirmingDiscard)
    {
      return;
    }

    if (!isLeftPress(mouse))
    {
      return;
    }

    if (auto const optRow = mouseRowAt(_rowBoxes, mouse); optRow && *optRow < _fields.size())
    {
      closeCompletion();
      _focusedField = *optRow;

      if (!_inputBoxes[*optRow].IsEmpty() && mouse.x >= _inputBoxes[*optRow].x_min)
      {
        _fields[*optRow].input.tryMoveToCell(mouse.x - _inputBoxes[*optRow].x_min);
      }

      return;
    }

    if (_optActiveCompletion)
    {
      if (auto const optRow = mouseRowAt(_candidateBoxes, mouse); optRow)
      {
        _completionSelection.select(*optRow);
        acceptSelectedCompletion();
        closeCompletion();
      }
    }
  }

  std::int32_t SmartListEditor::labelColumns() const
  {
    std::int32_t width = 0;

    for (auto const& field : _fields)
    {
      width = std::max(width, cellWidth(i18n::requiredText(_textCatalog, field.labelId)));
    }

    for (auto const labelId : {MessageId::TuiListInheritedExpression, MessageId::TuiListEffectiveExpression})
    {
      width = std::max(width, cellWidth(i18n::requiredText(_textCatalog, labelId)));
    }

    return std::max(1, std::min(width, kMaximumLabelColumns));
  }

  ftxui::Element SmartListEditor::renderFieldRow(std::size_t const index) const
  {
    using namespace ftxui;

    auto const& field = _fields[index];
    auto const focused = _focusedField == index;
    auto const label = std::string{i18n::requiredText(_textCatalog, field.labelId)};

    auto valuePtr = textFieldValue(field.input, &_inputBoxes[index], focused);

    if (field.input.empty() && field.placeholderId != MessageId::Count)
    {
      valuePtr = hbox({
        std::move(valuePtr),
        text(std::string{i18n::requiredText(_textCatalog, field.placeholderId)}) | style::muted(),
      });
    }

    auto cells = Elements{
      text(focused ? "> " : "  ") | (focused ? bold : style::muted()),
      text(fitCellText(label, labelColumns())),
      text(std::string(static_cast<std::size_t>(kLabelGapColumns), ' ')),
      std::move(valuePtr) | flex,
    };

    auto rowElementPtr = hbox(std::move(cells)) | ftxui::reflect(_rowBoxes[index]);

    if (focused && _optActiveCompletion)
    {
      // The popup opens below the input row with its border under the value
      // column, sharing the track editor's candidate rendering; this row has
      // no changed-marker gutter, so its indent is its own.
      rowElementPtr = vbox({
        std::move(rowElementPtr),
        renderCompletionPopup(_optActiveCompletion->items,
                              _completionSelection,
                              kFocusMarkerColumns + labelColumns() + kLabelGapColumns,
                              _candidateBoxes),
      });
    }

    return rowElementPtr;
  }

  ftxui::Element SmartListEditor::renderInheritedRows(std::int32_t const bodyColumns) const
  {
    using namespace ftxui;

    // A List under a filtered parent composes membership the user cannot see
    // from the local expression alone, so both halves are shown together, in
    // the field rows' label and value columns. A long expression says it was
    // shortened instead of running under the modal's border.
    auto const valueColumns = std::max(1, bodyColumns - kFocusMarkerColumns - labelColumns() - kLabelGapColumns);
    auto const renderRow = [this, valueColumns](MessageId const labelId, std::string expressionText)
    {
      auto label = std::string{i18n::requiredText(_textCatalog, labelId)};
      return hbox({
        text(std::string(static_cast<std::size_t>(kFocusMarkerColumns), ' ')),
        text(fitCellText(label, labelColumns())) | style::muted(),
        text(std::string(static_cast<std::size_t>(kLabelGapColumns), ' ')),
        text(ellipsizeToCellWidth(
          uimodel::formatSmartListExpressionDisplayText(_textCatalog, expressionText), valueColumns)) |
          flex,
      });
    };

    return vbox({
      renderRow(MessageId::TuiListInheritedExpression, _parentExpression),
      renderRow(MessageId::TuiListEffectiveExpression,
                uimodel::combineSmartListEffectiveExpression(_parentExpression, _fields[2].input.value())),
    });
  }

  ftxui::Element SmartListEditor::renderBody(std::int32_t const rowBudget, std::int32_t const bodyColumns) const
  {
    using namespace ftxui;

    _rowBoxes.assign(_fields.size(), kEmptyMouseBox);
    _inputBoxes.assign(_fields.size(), kEmptyMouseBox);
    _candidateBoxes.assign(_optActiveCompletion ? _optActiveCompletion->items.size() : 0, kEmptyMouseBox);

    // Every row except the sampled tracks is fixed: the fields, the open
    // popup, the inherited expression, and the status and guidance lines.
    // The tracks are only a sample, so they are the part a short terminal
    // gives up, ending in an ellipsis rather than silently losing the lines
    // below them.
    auto const showsError = _viewState.errorVisible && !_viewState.errorText.empty();
    auto fixedRows = static_cast<std::int32_t>(_fields.size());

    if (_optActiveCompletion)
    {
      fixedRows += kPopupBorderRows +
                   static_cast<std::int32_t>(std::min(_optActiveCompletion->items.size(), kCompletionPopupPageSize));
    }

    if (!_parentExpression.empty())
    {
      fixedRows += kInheritedRows;
    }

    // Each status, guidance, or error line is preceded by one blank spacer.
    for (auto const shown :
         {!_viewState.previewStatusText.empty(), !_viewState.membershipEditingText.empty(), showsError})
    {
      fixedRows += shown ? 2 : 0;
    }

    auto const trackBudget = static_cast<std::size_t>(std::max(0, rowBudget - fixedRows));
    auto const trackRows = std::min(_previewTracks.size(), trackBudget);
    auto const tracksClipped = trackRows < _previewTracks.size();

    auto rows = Elements{};

    for (std::size_t index = 0; index < _fields.size(); ++index)
    {
      rows.push_back(renderFieldRow(index));
    }

    if (!_parentExpression.empty())
    {
      rows.push_back(renderInheritedRows(bodyColumns));
    }

    if (!_viewState.previewStatusText.empty())
    {
      rows.push_back(text(""));
      rows.push_back(text(_viewState.previewStatusText) | bold);
    }

    for (std::size_t index = 0; index < trackRows; ++index)
    {
      auto const isLastClippedRow = tracksClipped && index + 1 == trackRows;
      rows.push_back(
        text(isLastClippedRow ? std::string{kCellEllipsis} : ellipsizeToCellWidth(_previewTracks[index], bodyColumns)) |
        style::muted());
    }

    if (!_viewState.membershipEditingText.empty())
    {
      rows.push_back(text(""));
      rows.push_back(text(_viewState.membershipEditingText) | style::muted());
    }

    if (showsError)
    {
      rows.push_back(text(""));
      rows.push_back(text(_viewState.errorText) | style::danger());
    }

    return vbox(std::move(rows));
  }

  std::string SmartListEditor::footerNotice() const
  {
    if (_confirmingDiscard)
    {
      return std::string{i18n::requiredText(_textCatalog, MessageId::TuiEditorDiscardPrompt)};
    }

    if (_status == ListEditorStatus::Submitting)
    {
      return {};
    }

    return _diagnostic;
  }

  std::int32_t SmartListEditor::footerRows(std::int32_t const bodyColumns) const
  {
    auto const notice = footerNotice();
    auto const noticeRows = notice.empty() ? 0 : static_cast<std::int32_t>(wrapCellText(notice, bodyColumns).size());
    return noticeRows + 1;
  }

  ftxui::Element SmartListEditor::renderFooter(std::int32_t const bodyColumns) const
  {
    using namespace ftxui;

    auto const notice = footerNotice();
    auto rows = notice.empty() ? Elements{} : wrappedRows(notice, bodyColumns);

    for (auto& rowPtr : rows)
    {
      rowPtr = std::move(rowPtr) | style::warning();
    }

    if (_confirmingDiscard)
    {
      rows.push_back(hbox(
        {_mouseBindings.bind(text(" [Enter] "), Event::Return), _mouseBindings.bind(text(" [Esc] "), Event::Escape)}));
      return vbox(std::move(rows));
    }

    if (_status == ListEditorStatus::Submitting)
    {
      return text(std::string{i18n::requiredText(_textCatalog, MessageId::TuiListStatusSaving)}) | bold;
    }

    auto chips = Elements{};

    if (hasCompletion())
    {
      chips.push_back(style::shortcutChip("Up/Down", i18n::requiredText(_textCatalog, MessageId::TuiEditorHintSelect)));
      chips.push_back(_mouseBindings.bind(
        style::shortcutChip("Enter", i18n::requiredText(_textCatalog, MessageId::TuiEditorHintAccept)), Event::Return));
      chips.push_back(_mouseBindings.bind(
        style::shortcutChip("Esc", i18n::requiredText(_textCatalog, MessageId::TuiEditorHintDismiss)), Event::Escape));
    }
    else
    {
      chips.push_back(_mouseBindings.bind(
        style::shortcutChip("Ctrl-N", i18n::requiredText(_textCatalog, MessageId::TuiEditorHintComplete)),
        Event::CtrlN));
      chips.push_back(_mouseBindings.bind(
        style::shortcutChip("Esc", i18n::requiredText(_textCatalog, MessageId::TuiEditorHintClose)), Event::Escape));
    }

    auto savePtr = _mouseBindings.bind(
      style::shortcutChip("Ctrl-S", std::string{i18n::requiredText(_textCatalog, MessageId::TuiListSaveAction)}),
      Event::CtrlS);
    chips.push_back(canSubmit() ? std::move(savePtr) : std::move(savePtr) | dim);

    auto withSeparators = Elements{};

    for (std::size_t index = 0; index < chips.size(); ++index)
    {
      if (index > 0)
      {
        withSeparators.push_back(style::mutedSeparator());
      }

      withSeparators.push_back(std::move(chips[index]));
    }

    rows.push_back(hbox(std::move(withSeparators)));
    return vbox(std::move(rows));
  }
} // namespace ao::tui
