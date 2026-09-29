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
#include <ao/uimodel/library/list/SmartListEditing.h>

#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ao::tui
{
  namespace
  {
    using i18n::MessageId;

    constexpr std::int32_t kMaximumLabelColumns = 16;
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
    auto const makeField = [](MessageId const labelId, std::string value, bool const completes)
    {
      auto field = FieldRow{.labelId = labelId, .input = TextFieldModel{value}, .completes = completes};
      field.baselineText = std::move(value);
      return field;
    };

    _fields.push_back(makeField(MessageId::TuiListFieldName, _baseline.name, false));
    _fields.push_back(makeField(MessageId::TuiListFieldDescription, _baseline.description, false));
    _fields.push_back(makeField(MessageId::TuiListFieldExpression, _baseline.expression, true));
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
      if (canSubmit())
      {
        _request = ListEditorRequest::Submit;
      }

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
             style::scrollablePanelBody(renderBody()),
             style::panelBody(separator()),
             style::panelBody(renderFooter()),
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

    auto boxPtr = render() | size(WIDTH, EQUAL, modalCols) | size(HEIGHT, EQUAL, modalRows) | clear_under;

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

    if (!explicitRequest && optResult->items.size() == 1 && optResult->items.front().insertText == field.input.value())
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

  ftxui::Element SmartListEditor::renderFieldRow(std::size_t const index) const
  {
    using namespace ftxui;

    auto const& field = _fields[index];
    auto const focused = _focusedField == index;
    auto const label = std::string{i18n::requiredText(_textCatalog, field.labelId)};

    auto cells = Elements{
      text(focused ? "> " : "  ") | (focused ? bold : style::muted()),
      text(fitCellText(label, kMaximumLabelColumns)),
      text(" "),
      textFieldValue(field.input, &_inputBoxes[index], focused) | flex,
    };

    auto rowElementPtr = hbox(std::move(cells)) | ftxui::reflect(_rowBoxes[index]);

    if (focused && _optActiveCompletion)
    {
      // The popup starts after the input row and floats above the preview
      // beneath it, sharing the track editor's candidate rendering.
      rowElementPtr = vbox({
        std::move(rowElementPtr),
        renderCompletionPopup(_optActiveCompletion->items,
                              _completionSelection,
                              kMaximumLabelColumns + static_cast<std::int32_t>(kCompletionPopupIndentColumns),
                              _candidateBoxes),
      });
    }

    return rowElementPtr;
  }

  ftxui::Element SmartListEditor::renderInheritedRows() const
  {
    using namespace ftxui;

    // A List under a filtered parent composes membership the user cannot see
    // from the local expression alone, so both halves are shown together.
    auto const renderRow = [this](MessageId const labelId, std::string expressionText)
    {
      auto label = std::string{i18n::requiredText(_textCatalog, labelId)};
      return hbox({
        text(fitCellText(label, kMaximumLabelColumns)) | style::muted(),
        text(" "),
        text(uimodel::formatSmartListExpressionDisplayText(_textCatalog, expressionText)) | flex,
      });
    };

    return vbox({
      renderRow(MessageId::TuiListInheritedExpression, _parentExpression),
      renderRow(MessageId::TuiListEffectiveExpression,
                uimodel::combineSmartListEffectiveExpression(_parentExpression, _fields[2].input.value())),
    });
  }

  ftxui::Element SmartListEditor::renderBody() const
  {
    using namespace ftxui;

    _rowBoxes.assign(_fields.size(), kEmptyMouseBox);
    _inputBoxes.assign(_fields.size(), kEmptyMouseBox);
    _candidateBoxes.assign(_optActiveCompletion ? _optActiveCompletion->items.size() : 0, kEmptyMouseBox);

    auto rows = Elements{};

    for (std::size_t index = 0; index < _fields.size(); ++index)
    {
      rows.push_back(renderFieldRow(index));
    }

    if (!_parentExpression.empty())
    {
      rows.push_back(renderInheritedRows());
    }

    if (!_viewState.previewStatusText.empty())
    {
      rows.push_back(text(""));
      rows.push_back(text(_viewState.previewStatusText) | bold);
    }

    for (auto const& previewTrack : _previewTracks)
    {
      rows.push_back(text(previewTrack) | style::muted());
    }

    if (!_viewState.membershipEditingText.empty())
    {
      rows.push_back(text(""));
      rows.push_back(text(_viewState.membershipEditingText) | style::muted());
    }

    if (_viewState.errorVisible)
    {
      rows.push_back(text(""));
      rows.push_back(text(_viewState.errorText) | style::danger());
    }

    return vbox(std::move(rows));
  }

  ftxui::Element SmartListEditor::renderFooter() const
  {
    using namespace ftxui;

    if (_confirmingDiscard)
    {
      return vbox(
        {paragraph(std::string{i18n::requiredText(_textCatalog, MessageId::TuiEditorDiscardPrompt)}) | style::warning(),
         hbox({_mouseBindings.bind(text(" [Enter] "), Event::Return),
               _mouseBindings.bind(text(" [Esc] "), Event::Escape)})});
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

    auto withSeparators = Elements{};

    for (std::size_t index = 0; index < chips.size(); ++index)
    {
      if (index > 0)
      {
        withSeparators.push_back(style::mutedSeparator());
      }

      withSeparators.push_back(std::move(chips[index]));
    }

    auto savePtr = _mouseBindings.bind(
      style::shortcutChip("Ctrl-S", std::string{i18n::requiredText(_textCatalog, MessageId::TuiListSaveAction)}),
      Event::CtrlS);
    withSeparators.push_back(canSubmit() ? std::move(savePtr) : std::move(savePtr) | dim);

    if (!_diagnostic.empty())
    {
      return vbox({
        paragraph(_diagnostic) | style::warning(),
        hbox(std::move(withSeparators)),
      });
    }

    return hbox(std::move(withSeparators));
  }
} // namespace ao::tui
