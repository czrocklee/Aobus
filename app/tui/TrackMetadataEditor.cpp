// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "TrackMetadataEditor.h"

#include "MouseBindings.h"
#include "SelectableList.h"
#include "SelectionNavigation.h"
#include "Style.h"
#include "TextCell.h"
#include "TextField.h"
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/TrackField.h>
#include <ao/uimodel/library/property/TrackPropertiesFormModel.h>
#include <ao/uimodel/library/property/TrackPropertiesFormSpec.h>
#include <ao/uimodel/library/track/TrackAuthoring.h>

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::tui
{
  namespace
  {
    using i18n::MessageId;
    constexpr std::int32_t kMaximumLabelColumns = 18;
    constexpr std::size_t kCompletionPageSize = 6;

    bool isNumberRow(uimodel::TrackPropertiesFormRow const& row) noexcept
    {
      return row.editorKind == uimodel::TrackPropertiesFormEditorKind::Number;
    }

    /// Whether @p text parses under the field's own codec, which is what Save will use.
    bool isValidForRow(uimodel::TrackPropertiesFormRow const& row, std::string_view const text)
    {
      return isNumberRow(row) ? uimodel::parseUint16EditValue(text).has_value()
                              : uimodel::parseTextEditValue(text).has_value();
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
                                           std::size_t const targetCount,
                                           uimodel::TrackPropertiesFormModel baseline,
                                           CompletionProvider completionProvider)
    : _textCatalog{std::move(textCatalog)}
    , _targetCount{targetCount}
    , _baseline{std::move(baseline)}
    , _spec{uimodel::buildTrackPropertiesFormSpec(_textCatalog)}
    , _completionProvider{std::move(completionProvider)}
  {
    _metadataRows.reserve(_spec.metadataRows.size());

    for (auto const& specRow : _spec.metadataRows)
    {
      auto const view = _baseline.rowView(specRow.field);
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
  }

  bool TrackMetadataEditor::isDirty() const noexcept
  {
    return std::ranges::any_of(_metadataRows, [](auto const& row) { return row.isIncluded(); });
  }

  bool TrackMetadataEditor::hasInvalidFields() const noexcept
  {
    return std::ranges::any_of(_metadataRows, [](auto const& row) { return row.invalid; });
  }

  std::size_t TrackMetadataEditor::editedFieldCount() const noexcept
  {
    return static_cast<std::size_t>(
      std::ranges::count_if(_metadataRows, [](auto const& row) { return row.isIncluded(); }));
  }

  std::size_t TrackMetadataEditor::clearedFieldCount() const noexcept
  {
    return static_cast<std::size_t>(
      std::ranges::count_if(_metadataRows, [](auto const& row) { return row.intent == FieldIntent::ExplicitClear; }));
  }

  rt::MetadataPatch TrackMetadataEditor::buildPatch() const
  {
    auto form = _baseline;

    for (auto const& row : _metadataRows)
    {
      if (row.intent == FieldIntent::Unchanged)
      {
        continue;
      }

      if (row.intent == FieldIntent::ExplicitClear)
      {
        if (isNumberRow(row.spec))
        {
          form.setExplicitFieldEdit(row.spec.field, static_cast<std::uint16_t>(0));
        }
        else
        {
          form.setExplicitFieldEdit(row.spec.field, std::string{});
        }

        continue;
      }

      if (isNumberRow(row.spec))
      {
        if (auto const valueRes = uimodel::parseUint16EditValue(row.input.value()); valueRes)
        {
          form.setExplicitFieldEdit(row.spec.field, *valueRes);
        }

        continue;
      }

      if (auto const valueRes = uimodel::parseTextEditValue(row.input.value()); valueRes)
      {
        form.setExplicitFieldEdit(row.spec.field, *valueRes);
      }
    }

    return form.buildPatch();
  }

  bool TrackMetadataEditor::supportsCompletion() const noexcept
  {
    return _focusedMetadataRow < _metadataRows.size() &&
           rt::supportsTrackFieldValueCompletion(_metadataRows[_focusedMetadataRow].spec.field);
  }

  void TrackMetadataEditor::closeCompletion()
  {
    _candidateBoxes.clear();
    _optActiveCompletion.reset();
    _selectedCandidate = 0;
    _completionWindowStart = 0;
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
          _selectedCandidate = *optRow;
          return tryHandleCompletionEvent(ftxui::Event::Return);
        }
      }

      return false;
    }

    if (tryHandleCompletionNavigation(event, _optActiveCompletion->items.size()))
    {
      return true;
    }

    if (event == ftxui::Event::Return)
    {
      if (_optActiveCompletion && _selectedCandidate < _optActiveCompletion->items.size() &&
          _focusedMetadataRow < _metadataRows.size())
      {
        auto& row = _metadataRows[_focusedMetadataRow];
        auto const& item = _optActiveCompletion->items[_selectedCandidate];

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
    if (_metadataRows.empty() || _focusedMetadataRow >= _metadataRows.size())
    {
      return;
    }

    if (event.is_mouse())
    {
      auto mouseEvent = event;

      if (auto const& mouse = mouseEvent.mouse(); isLeftPress(mouse))
      {
        if (auto const optRow = mouseRowAt(_rowBoxes, mouse); optRow && *optRow < _metadataRows.size())
        {
          closeCompletion();
          _focusedMetadataRow = *optRow;

          if (!_inputBoxes[*optRow].IsEmpty() && mouse.x >= _inputBoxes[*optRow].x_min)
          {
            _metadataRows[*optRow].input.tryMoveToCell(mouse.x - _inputBoxes[*optRow].x_min);
          }
        }
      }

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

    auto& row = _metadataRows[_focusedMetadataRow];

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
    std::int32_t labelColumns = 0;

    for (auto const& row : _metadataRows)
    {
      labelColumns = std::max(labelColumns, cellWidth(row.spec.label));
    }

    return renderRows(std::min(labelColumns, kMaximumLabelColumns));
  }

  ftxui::Element TrackMetadataEditor::renderProperties() const
  {
    using namespace ftxui;

    std::int32_t labelColumns = 0;

    for (auto const& specRow : _spec.propertyRows)
    {
      labelColumns = std::max(labelColumns, cellWidth(specRow.label));
    }

    labelColumns = std::min(labelColumns, kMaximumLabelColumns);

    auto rows = std::vector<SelectableListRow>{};
    rows.reserve(_spec.propertyRows.size());

    for (auto const& specRow : _spec.propertyRows)
    {
      auto const view = _baseline.rowView(specRow.field);
      auto valuePtr = text(view.text);

      rows.push_back(SelectableListRow{
        .elementPtr = hbox({
          text(fitCellText(specRow.label, labelColumns)) | style::muted(),
          text("  "),
          view.mixed ? std::move(valuePtr) | style::muted() : std::move(valuePtr),
        }),
      });
    }

    return selectableList(std::move(rows),
                          SelectableListOptions{.focusRow = static_cast<std::int32_t>(_readonlyRow),
                                                .horizontalScroll = false,
                                                .flex = true,
                                                .viewportBox = &_propertiesViewport});
  }

  void TrackMetadataEditor::moveMetadataRow(std::int32_t const delta)
  {
    if (_metadataRows.empty())
    {
      return;
    }

    closeCompletion();
    auto const last = static_cast<std::int32_t>(_metadataRows.size()) - 1;
    auto const target = std::clamp(static_cast<std::int32_t>(_focusedMetadataRow) + delta, 0, last);
    _focusedMetadataRow = static_cast<std::size_t>(target);
  }

  void TrackMetadataEditor::scrollProperties(std::int32_t const delta)
  {
    auto const count = static_cast<std::int32_t>(_spec.propertyRows.size());

    if (count == 0)
    {
      return;
    }

    auto const target = moveSelection(static_cast<std::int32_t>(_readonlyRow), delta, static_cast<std::size_t>(count));
    _readonlyRow = static_cast<std::size_t>(target);
  }

  void TrackMetadataEditor::clearField(MetadataRow& row)
  {
    row.input.reset("");
    row.intent = FieldIntent::ExplicitClear;
    revalidate(row);
  }

  void TrackMetadataEditor::restoreField(MetadataRow& row)
  {
    row.intent = FieldIntent::Unchanged;
    row.invalid = false;
    row.input.reset(row.mixed ? "" : row.baselineText);
  }

  void TrackMetadataEditor::noteRowEdited(MetadataRow& row)
  {
    if (!row.mixed && row.input.value() == row.baselineText)
    {
      row.intent = FieldIntent::Unchanged;
    }
    else if (row.input.empty())
    {
      row.intent = FieldIntent::ExplicitClear;
    }
    else
    {
      row.intent = FieldIntent::Replacement;
    }

    revalidate(row);
  }

  void TrackMetadataEditor::revalidate(MetadataRow& row)
  {
    row.invalid = (row.intent != FieldIntent::Unchanged) && !isValidForRow(row.spec, row.input.value());
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
    _selectedCandidate = 0;
    _completionWindowStart = 0;
  }

  bool TrackMetadataEditor::tryHandleCompletionNavigation(ftxui::Event const& event, std::size_t const itemCount)
  {
    std::int32_t delta = 0;
    auto const pageSize = static_cast<std::int32_t>(kCompletionPageSize);

    if (event == ftxui::Event::ArrowUp)
    {
      delta = -1;
    }
    else if (event == ftxui::Event::ArrowDown)
    {
      delta = 1;
    }
    else if (event == ftxui::Event::PageUp)
    {
      delta = -pageSize;
    }
    else if (event == ftxui::Event::PageDown)
    {
      delta = pageSize;
    }
    else
    {
      return false;
    }

    _selectedCandidate =
      static_cast<std::size_t>(moveSelection(static_cast<std::int32_t>(_selectedCandidate), delta, itemCount));

    if (event == ftxui::Event::PageUp || event == ftxui::Event::PageDown)
    {
      auto const windowCount = itemCount - std::min(itemCount, kCompletionPageSize) + 1;
      _completionWindowStart =
        static_cast<std::size_t>(moveSelection(static_cast<std::int32_t>(_completionWindowStart), delta, windowCount));
    }

    if (_selectedCandidate < _completionWindowStart)
    {
      _completionWindowStart = _selectedCandidate;
    }
    else if (_selectedCandidate >= _completionWindowStart + kCompletionPageSize)
    {
      _completionWindowStart = _selectedCandidate - kCompletionPageSize + 1;
    }

    return true;
  }

  ftxui::Element TrackMetadataEditor::renderFieldValue(MetadataRow const& row,
                                                       bool const focused,
                                                       ftxui::Box& inputBox) const
  {
    using namespace ftxui;

    if (row.intent == FieldIntent::ExplicitClear)
    {
      auto clearPtr = text(countedText(_textCatalog, MessageId::TuiEditorClearForAll, _targetCount)) | style::warning();
      return focused ? hbox({emptyInputCaret(), std::move(clearPtr)}) : std::move(clearPtr);
    }

    if (row.intent == FieldIntent::Unchanged && row.mixed && row.input.empty())
    {
      auto markerPtr =
        text(std::string{i18n::requiredText(_textCatalog, MessageId::TrackMultipleValues)}) | style::muted();
      return focused ? hbox({emptyInputCaret(), std::move(markerPtr)}) : std::move(markerPtr);
    }

    return textFieldValue(row.input, &inputBox, focused);
  }

  ftxui::Element TrackMetadataEditor::renderRows(std::int32_t const labelColumns) const
  {
    using namespace ftxui;

    auto rows = std::vector<SelectableListRow>{};
    rows.reserve(_metadataRows.size());
    _rowBoxes.assign(_metadataRows.size(), kEmptyMouseBox);
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
        cells.push_back(text(" "));
        cells.push_back(text(std::string{i18n::requiredText(_textCatalog, MessageId::TuiEditorInvalidNumber)}) |
                        style::danger());
      }

      auto rowElementPtr = hbox(std::move(cells)) | ftxui::reflect(_rowBoxes[index]);

      if (rowFocused && _optActiveCompletion)
      {
        auto const totalItems = _optActiveCompletion->items.size();
        auto const windowStart = _completionWindowStart;
        auto const windowEnd = std::min(totalItems, windowStart + kCompletionPageSize);
        // The popup starts after the input and its top border. Scroll to the
        // selected candidate even when a short terminal cannot show it all.
        focusLine += 2 + static_cast<std::int32_t>(_selectedCandidate - windowStart);

        auto candidateElements = Elements{};

        for (std::size_t candIndex = windowStart; candIndex < windowEnd; ++candIndex)
        {
          auto const& item = _optActiveCompletion->items[candIndex];
          auto const isCandidateSelected = candIndex == _selectedCandidate;
          auto indicatorPtr = text(isCandidateSelected ? "> " : "  ");
          auto itemTextPtr = text(item.displayText);
          auto itemRowPtr = hbox({std::move(indicatorPtr), std::move(itemTextPtr)});

          if (isCandidateSelected)
          {
            itemRowPtr = std::move(itemRowPtr) | inverted;
          }

          candidateElements.push_back(std::move(itemRowPtr) | ftxui::reflect(_candidateBoxes[candIndex]));
        }

        auto popupBoxPtr = style::panelBody(vbox(std::move(candidateElements))) | border | clear_under;
        auto const indentSize = static_cast<std::size_t>(labelColumns) + 5;
        auto indentedPopupPtr = hbox({
          text(std::string(indentSize, ' ')),
          std::move(popupBoxPtr),
        });

        rowElementPtr = vbox({
          std::move(rowElementPtr),
          std::move(indentedPopupPtr),
        });
      }

      rows.push_back(SelectableListRow{.elementPtr = std::move(rowElementPtr)});
    }

    return selectableList(
      std::move(rows),
      SelectableListOptions{
        .focusRow = focusLine, .horizontalScroll = false, .flex = true, .viewportBox = &_metadataViewport});
  }
} // namespace ao::tui
