// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "TrackMetadataEditor.h"

#include "CompletionPopup.h"
#include "MouseBindings.h"
#include "SelectableList.h"
#include "SelectionNavigation.h"
#include "Style.h"
#include "TextCell.h"
#include "TextField.h"
#include "TrackCreditsEditor.h"
#include <ao/i18n/MessageCatalog.h>
#include <ao/library/Credits.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackMutation.h>
#include <ao/uimodel/library/detail/TrackCredits.h>
#include <ao/uimodel/library/property/TrackPropertiesFormModel.h>
#include <ao/uimodel/library/property/TrackPropertiesFormSpec.h>

#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>

#include <algorithm>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace ao::tui
{
  namespace
  {
    using i18n::MessageId;
    constexpr std::int32_t kMaximumLabelColumns = 18;

    bool isEditable(uimodel::TrackPropertiesFormRow const& row) noexcept
    {
      return row.editorKind != uimodel::TrackPropertiesFormEditorKind::ReadonlyText;
    }

    std::string countedText(i18n::MessageCatalog const& textCatalog, MessageId const id, std::size_t const count)
    {
      return i18n::requiredFormat(textCatalog, id, {{"count", count}});
    }

    /// One inverted cell standing in for the caret of an empty input.
    ftxui::Element emptyInputCaret()
    {
      using namespace ftxui;

      return text(" ") | inverted;
    }
  } // namespace

  TrackMetadataEditor::TrackMetadataEditor(i18n::MessageCatalog textCatalog,
                                           uimodel::TrackPropertiesFormModel baseline,
                                           CompletionProvider completionProvider,
                                           TrackCreditsEditor::CompletionProvider creditCompletionProvider)
    : _textCatalog{std::move(textCatalog)}
    , _form{std::move(baseline)}
    , _creditsEditor{_textCatalog, std::move(creditCompletionProvider)}
    , _spec{uimodel::buildTrackPropertiesFormSpec(_textCatalog)}
    , _completionProvider{std::move(completionProvider)}
  {
    _metadataRows.reserve(_spec.metadataRows.size());

    for (auto const& specRow : _spec.metadataRows)
    {
      auto const view = _form.rowView(specRow.field);
      auto row = MetadataRow{.spec = specRow, .mixed = view.mixed};

      // A mixed field starts empty so its marker can be presentation-only. A
      // common field starts at the value every target already agrees on.
      if (!view.mixed)
      {
        row.baselineText = view.text;
        row.input.reset(view.text);
      }

      _metadataRows.push_back(std::move(row));
    }

    // A baseline prepared with a begun Credits session opens in that session.
    if (_form.creditsEditor().isEditing())
    {
      _creditsEditor.reset(_form.creditsEditor());
    }
  }

  bool TrackMetadataEditor::isDirty() const noexcept
  {
    return _form.pendingCredits().has_value() || _form.creditsEditor().hasDraftChanges() ||
           std::ranges::any_of(_metadataRows, [](auto const& row) { return row.isIncluded(); });
  }

  void TrackMetadataEditor::beginCreditsEdit(std::bitset<library::kCreditKindCount> const kinds)
  {
    closeCompletion();

    // A refused baseline stays visible; otherwise the key would look ignored.
    if (auto res = _form.beginCreditsEdit(kinds); !res)
    {
      _creditsOpenError = std::move(res.error().message);
      return;
    }

    _creditsOpenError.clear();
    _creditsEditor.reset(_form.creditsEditor());
  }

  void TrackMetadataEditor::handleCreditsEvent(ftxui::Event const& event)
  {
    switch (_creditsEditor.handleEvent(event, _form.creditsEditor()))
    {
      case TrackCreditsEditor::Request::None: break;
      case TrackCreditsEditor::Request::Cancel: _form.cancelCreditsEdit(); break;
      case TrackCreditsEditor::Request::Accept: std::ignore = _form.acceptCreditsEdit(); break;
    }
  }

  bool TrackMetadataEditor::hasInvalidFields() const noexcept
  {
    return std::ranges::any_of(_metadataRows, [](auto const& row) { return row.invalid; });
  }

  std::size_t TrackMetadataEditor::editedFieldCount() const noexcept
  {
    return static_cast<std::size_t>(
             std::ranges::count_if(_metadataRows, [](auto const& row) { return row.isIncluded(); })) +
           (_form.pendingCredits() ? 1 : 0);
  }

  std::size_t TrackMetadataEditor::clearedFieldCount() const noexcept
  {
    return static_cast<std::size_t>(std::ranges::count_if(
             _metadataRows, [](auto const& row) { return row.intent == FieldIntent::ExplicitClear; })) +
           (_form.pendingCredits() && _form.pendingCredits()->entries.empty() ? 1 : 0);
  }

  rt::MetadataPatch TrackMetadataEditor::buildPatch() const
  {
    return _form.buildPatch();
  }

  bool TrackMetadataEditor::supportsCompletion() const noexcept
  {
    return _focusedMetadataRow < _metadataRows.size() &&
           !rt::creditKindForTrackField(_metadataRows[_focusedMetadataRow].spec.field) &&
           rt::supportsTrackFieldValueCompletion(_metadataRows[_focusedMetadataRow].spec.field);
  }

  std::optional<std::string> TrackMetadataEditor::focusedClearLabel() const
  {
    if (_focusedMetadataRow >= _metadataRows.size() || !isEditable(_metadataRows[_focusedMetadataRow].spec))
    {
      return std::nullopt;
    }

    auto const& row = _metadataRows[_focusedMetadataRow].spec;
    return i18n::requiredFormat(
      _textCatalog, MessageId::TrackClearFieldForTracks, {{"field", row.label}, {"count", targetCount()}});
  }

  void TrackMetadataEditor::closeCompletion()
  {
    _candidateBoxes.clear();
    _optActiveCompletion.reset();
    _completionSelection.reset();
  }

  bool TrackMetadataEditor::tryHandleCompletionEvent(ftxui::Event const& event)
  {
    if (!_optActiveCompletion)
    {
      return false;
    }

    if (event.is_mouse())
    {
      auto mouseEvent = event;

      if (auto const& mouse = mouseEvent.mouse(); isLeftPress(mouse))
      {
        if (auto const optRow = mouseRowAt(_candidateBoxes, mouse); optRow)
        {
          _completionSelection.select(*optRow);
          return tryHandleCompletionEvent(ftxui::Event::Return);
        }
      }

      return false;
    }

    if (_completionSelection.tryNavigate(event, _optActiveCompletion->items.size()))
    {
      return true;
    }

    if (event == ftxui::Event::Return)
    {
      if (_optActiveCompletion && _completionSelection.selectedCandidate() < _optActiveCompletion->items.size() &&
          _focusedMetadataRow < _metadataRows.size())
      {
        auto& row = _metadataRows[_focusedMetadataRow];
        auto const& item = _optActiveCompletion->items[_completionSelection.selectedCandidate()];

        if (auto const beforeVal = row.input.value(); row.input.tryReplaceRange(
              _optActiveCompletion->replaceBegin, _optActiveCompletion->replaceEnd, item.insertText))
        {
          if (row.input.value() != beforeVal)
          {
            noteRowEdited(row);
          }
        }
      }

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
        event == ftxui::Event::Special("\033b") || event == ftxui::Event::Special("\033f") ||
        event == ftxui::Event::ArrowLeftCtrl || event == ftxui::Event::ArrowRightCtrl)
    {
      closeCompletion();
      return false;
    }

    if (event == ftxui::Event::CtrlS || event == ftxui::Event::CtrlR || event == ftxui::Event::CtrlD ||
        event == ftxui::Event::CtrlG || event == ftxui::Event::Tab || event == ftxui::Event::TabReverse)
    {
      closeCompletion();
      return false;
    }

    return false;
  }

  void TrackMetadataEditor::handleEvent(ftxui::Event const& event)
  {
    if (isEditingCredits())
    {
      handleCreditsEvent(event);
      return;
    }

    if (event == ftxui::Event::CtrlO)
    {
      beginCreditsEdit(uimodel::allTrackCreditKinds());
      return;
    }

    if (event.is_mouse())
    {
      auto mouseEvent = event;
      handleMetadataMouse(mouseEvent.mouse());
      return;
    }

    if (event == ftxui::Event::ArrowUp)
    {
      moveMetadataRow(-1);
      return;
    }

    if (event == ftxui::Event::ArrowDown)
    {
      moveMetadataRow(1);
      return;
    }

    if (event == ftxui::Event::PageUp || event == ftxui::Event::PageDown)
    {
      moveMetadataRow(listNavigationDelta(event, navigationPageRows(_metadataViewport)).value_or(0));
      return;
    }

    if (_focusedMetadataRow == _metadataRows.size())
    {
      if (event == ftxui::Event::Return)
      {
        beginCreditsEdit(uimodel::allTrackCreditKinds());
      }

      return;
    }

    auto& row = _metadataRows[_focusedMetadataRow];

    if (auto const optKind = rt::creditKindForTrackField(row.spec.field); optKind)
    {
      if (event == ftxui::Event::Return)
      {
        beginCreditsEdit(uimodel::trackCreditScope(*optKind));
      }

      return;
    }

    if (!isEditable(row.spec))
    {
      return;
    }

    if (event == ftxui::Event::CtrlD)
    {
      clearField(row);
      closeCompletion();
      return;
    }

    if (event == ftxui::Event::CtrlG)
    {
      restoreField(row);
      closeCompletion();
      return;
    }

    if (event == ftxui::Event::CtrlN)
    {
      maybeTriggerCompletion(row, true);
      return;
    }

    if (auto const previousCursor = row.input.cursor(); row.input.tryApplyEvent(event))
    {
      noteRowEdited(row);
      maybeTriggerCompletion(row, false);
    }
    else if (row.input.cursor() != previousCursor)
    {
      closeCompletion();
    }
  }

  void TrackMetadataEditor::handlePropertiesEvent(ftxui::Event const& event)
  {
    if (auto optDelta = listNavigationDelta(event, navigationPageRows(_propertiesViewport), true); optDelta)
    {
      scrollProperties(*optDelta);
    }
  }

  ftxui::Element TrackMetadataEditor::render() const
  {
    if (isEditingCredits())
    {
      return _creditsEditor.render(_form.creditsEditor());
    }

    std::int32_t labelColumns = 0;

    for (auto const& row : _metadataRows)
    {
      labelColumns = std::max(labelColumns, cellWidth(row.spec.label));
    }

    auto rowsPtr = renderRows(std::min(labelColumns, kMaximumLabelColumns));

    if (_creditsOpenError.empty())
    {
      return rowsPtr;
    }

    return ftxui::vbox({std::move(rowsPtr), ftxui::paragraph(_creditsOpenError) | style::danger()});
  }

  ftxui::Element TrackMetadataEditor::renderProperties() const
  {
    using namespace ftxui;

    std::int32_t labelColumns = 0;

    for (auto const& specRow : _spec.propertyRows)
    {
      labelColumns = std::max(labelColumns, cellWidth(specRow.label));
    }

    for (auto const& row : _form.segmentRows())
    {
      labelColumns = std::max(labelColumns, cellWidth(row.label));
    }

    labelColumns = std::min(labelColumns, kMaximumLabelColumns);

    auto rows = std::vector<SelectableListRow>{};
    rows.reserve(_spec.propertyRows.size() + _form.segmentRows().size());

    for (auto const& specRow : _spec.propertyRows)
    {
      auto const view = _form.rowView(specRow.field);
      auto valuePtr = text(view.text);

      rows.push_back(SelectableListRow{
        .elementPtr = hbox({
          text(fitCellText(specRow.label, labelColumns)) | style::muted(),
          text("  "),
          view.mixed ? std::move(valuePtr) | style::muted() : std::move(valuePtr),
        }),
      });
    }

    for (auto const& row : _form.segmentRows())
    {
      rows.push_back(SelectableListRow{
        .elementPtr = hbox({text(fitCellText(row.label, labelColumns)) | style::muted(), text("  "), text(row.text)}),
      });
    }

    return selectableList(std::move(rows),
                          SelectableListOptions{.focusRow = static_cast<std::int32_t>(_readonlyRow),
                                                .horizontalScroll = false,
                                                .flex = true,
                                                .viewportBox = &_propertiesViewport});
  }

  void TrackMetadataEditor::handleMetadataMouse(ftxui::Mouse const& mouse)
  {
    if (!isLeftPress(mouse))
    {
      return;
    }

    auto const optRow = mouseRowAt(_rowBoxes, mouse);

    if (!optRow || *optRow > _metadataRows.size())
    {
      return;
    }

    closeCompletion();
    _focusedMetadataRow = *optRow;

    if (*optRow == _metadataRows.size())
    {
      beginCreditsEdit(uimodel::allTrackCreditKinds());
    }
    else if (auto const optKind = rt::creditKindForTrackField(_metadataRows[*optRow].spec.field); optKind)
    {
      beginCreditsEdit(uimodel::trackCreditScope(*optKind));
    }
    else if (!_inputBoxes[*optRow].IsEmpty() && mouse.x >= _inputBoxes[*optRow].x_min)
    {
      _metadataRows[*optRow].input.tryMoveToCell(mouse.x - _inputBoxes[*optRow].x_min);
    }
  }

  void TrackMetadataEditor::moveMetadataRow(std::int32_t const delta)
  {
    if (_metadataRows.empty())
    {
      return;
    }

    closeCompletion();
    auto const last = static_cast<std::int32_t>(_metadataRows.size());
    auto const target = std::clamp(static_cast<std::int32_t>(_focusedMetadataRow) + delta, 0, last);
    _focusedMetadataRow = static_cast<std::size_t>(target);
  }

  void TrackMetadataEditor::scrollProperties(std::int32_t const delta)
  {
    auto const count = static_cast<std::int32_t>(_spec.propertyRows.size() + _form.segmentRows().size());

    if (count == 0)
    {
      return;
    }

    auto const target = moveSelection(static_cast<std::int32_t>(_readonlyRow), delta, static_cast<std::size_t>(count));
    _readonlyRow = static_cast<std::size_t>(target);
  }

  void TrackMetadataEditor::clearField(MetadataRow& row)
  {
    _form.clearFieldEdit(row.spec.field);
    row.input.reset("");
    row.intent = _form.hasFieldChange(row.spec.field) ? FieldIntent::ExplicitClear : FieldIntent::Unchanged;
    row.invalid = false;
  }

  void TrackMetadataEditor::restoreField(MetadataRow& row)
  {
    _form.restoreFieldEdit(row.spec.field);
    row.intent = FieldIntent::Unchanged;
    row.invalid = false;
    row.input.reset(row.mixed ? "" : row.baselineText);
  }

  void TrackMetadataEditor::noteRowEdited(MetadataRow& row)
  {
    auto valueRes = uimodel::parseTrackPropertiesFieldInput(row.spec.editorKind, row.input.value());
    row.invalid = !valueRes;

    if (!valueRes)
    {
      // Keep invalid input available to fix, but never keep a stale edit in the patch.
      _form.restoreFieldEdit(row.spec.field);
      row.intent = FieldIntent::Replacement;
      return;
    }

    if (!row.mixed && row.input.value() == row.baselineText)
    {
      _form.restoreFieldEdit(row.spec.field);
      row.intent = FieldIntent::Unchanged;
      return;
    }

    _form.setFieldInput(row.spec.field, std::move(*valueRes));

    if (!_form.hasFieldChange(row.spec.field))
    {
      row.intent = FieldIntent::Unchanged;
    }
    else
    {
      row.intent = row.input.empty() ? FieldIntent::ExplicitClear : FieldIntent::Replacement;
    }
  }

  void TrackMetadataEditor::maybeTriggerCompletion(MetadataRow const& row, bool const explicitRequest)
  {
    if (!_completionProvider)
    {
      closeCompletion();
      return;
    }

    if (!explicitRequest && row.input.empty())
    {
      closeCompletion();
      return;
    }

    auto optRes = _completionProvider(row.spec.field, row.input.value(), row.input.cursor());

    if (!optRes || optRes->items.empty())
    {
      closeCompletion();
      return;
    }

    if (!explicitRequest && optRes->items.size() == 1 && optRes->items.front().insertText == row.input.value())
    {
      closeCompletion();
      return;
    }

    _optActiveCompletion = std::move(optRes);
    _completionSelection.reset();
  }

  ftxui::Element TrackMetadataEditor::renderFieldValue(MetadataRow const& row,
                                                       bool const focused,
                                                       ftxui::Box& inputBox) const
  {
    using namespace ftxui;

    if (auto const optKind = rt::creditKindForTrackField(row.spec.field); optKind)
    {
      auto const sections = _form.creditSections();
      auto const& section = sections[static_cast<std::size_t>(*optKind)];
      auto const value = uimodel::formatTrackCreditSectionSummary(_textCatalog, section);
      return hbox({text(value) | flex, text(" [Enter]") | dim});
    }

    if (row.intent == FieldIntent::ExplicitClear)
    {
      auto clearPtr =
        text(countedText(_textCatalog, MessageId::TuiEditorClearForAll, targetCount())) | style::warning();
      return focused ? hbox({emptyInputCaret(), std::move(clearPtr)}) : std::move(clearPtr);
    }

    if (targetCount() > 1 && row.intent == FieldIntent::Unchanged && (row.input.empty() || !focused) &&
        (row.mixed || row.input.value() != row.baselineText))
    {
      // Tell the user what will be preserved without inserting the displayed
      // value into their empty input or making them erase it again.
      auto markerPtr = text(row.mixed ? std::string{i18n::requiredText(_textCatalog, MessageId::TrackMultipleValues)}
                                      : row.baselineText) |
                       style::muted();
      return focused ? hbox({emptyInputCaret(), std::move(markerPtr)}) : std::move(markerPtr);
    }

    return textFieldValue(row.input, &inputBox, focused);
  }

  ftxui::Element TrackMetadataEditor::renderRows(std::int32_t const labelColumns) const
  {
    using namespace ftxui;

    auto rows = std::vector<SelectableListRow>{};
    rows.reserve(_metadataRows.size());
    _rowBoxes.assign(_metadataRows.size() + 1, kEmptyMouseBox);
    _inputBoxes.assign(_metadataRows.size(), kEmptyMouseBox);
    _candidateBoxes.assign(_optActiveCompletion ? _optActiveCompletion->items.size() : 0, kEmptyMouseBox);
    auto focusLine = static_cast<std::int32_t>(_focusedMetadataRow);

    for (std::size_t index = 0; index < _metadataRows.size(); ++index)
    {
      auto const& row = _metadataRows[index];
      auto const rowFocused = _focusedMetadataRow == index;

      auto activePtr = text(rowFocused ? "> " : "  ") | (rowFocused ? bold : style::muted());
      auto changedPtr = text(row.intent != FieldIntent::Unchanged ? "* " : "  ") | style::warning();

      auto cells = Elements{
        std::move(activePtr),
        std::move(changedPtr),
        text(fitCellText(row.spec.label, labelColumns)) | (row.intent != FieldIntent::Unchanged ? bold : nothing),
        text(" "),
        renderFieldValue(row, rowFocused, _inputBoxes[index]) | flex,
      };

      if (row.invalid)
      {
        auto const hint = uimodel::trackPropertiesInputHint(_textCatalog, row.spec.editorKind);
        cells.push_back(text(" "));
        cells.push_back(text(std::string{hint}) | style::danger());
      }

      auto rowElementPtr = hbox(std::move(cells)) | ftxui::reflect(_rowBoxes[index]);

      if (rowFocused && _optActiveCompletion)
      {
        // The popup starts after the input and its top border. Scroll to the
        // selected candidate even when a short terminal cannot show it all.
        focusLine +=
          2 + static_cast<std::int32_t>(_completionSelection.selectedCandidate() - _completionSelection.windowStart());

        rowElementPtr = vbox({
          std::move(rowElementPtr),
          renderCompletionPopup(_optActiveCompletion->items,
                                _completionSelection,
                                labelColumns + static_cast<std::int32_t>(kCompletionPopupIndentColumns),
                                _candidateBoxes),
        });
      }

      rows.push_back(SelectableListRow{.elementPtr = std::move(rowElementPtr)});
    }

    rows.push_back({.elementPtr = hbox({text(_focusedMetadataRow == _metadataRows.size() ? "> " : "  "),
                                        text(_spec.creditsLabel),
                                        text(" [Enter / Ctrl-O]") | dim}) |
                                  ftxui::reflect(_rowBoxes.back())});

    return selectableList(
      std::move(rows),
      SelectableListOptions{
        .focusRow = focusLine, .horizontalScroll = false, .flex = true, .viewportBox = &_metadataViewport});
  }
} // namespace ao::tui
