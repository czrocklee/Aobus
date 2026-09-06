// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "TrackPropertiesEditor.h"

#include "SelectableList.h"
#include "SelectionNavigation.h"
#include "Style.h"
#include "TextCell.h"
#include <ao/Contract.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackMutation.h>
#include <ao/uimodel/library/property/TrackPropertiesFormModel.h>
#include <ao/uimodel/library/property/TrackPropertiesFormSpec.h>
#include <ao/uimodel/library/track/TrackAuthoring.h>
#include <ao/utility/String.h>
#include <ao/utility/UnicodeText.h>

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::tui
{
  namespace
  {
    using i18n::MessageId;

    // FTXUI reports Ctrl-key chords as their C0 control characters.
    auto const kApplyEvent = ftxui::Event::Character(static_cast<char>(0x13));
    auto const kReloadEvent = ftxui::Event::Character(static_cast<char>(0x12));
    auto const kClearEvent = ftxui::Event::Character(static_cast<char>(0x15));
    auto const kRestoreEvent = ftxui::Event::Character(static_cast<char>(0x07));
    auto const kCompleteEvent = ftxui::Event::Character(static_cast<char>(0x0e));

    constexpr std::int32_t kMaximumLabelColumns = 18;
    /// Terminal lines one target occupies on the Tracks page: its title, then its path.
    constexpr std::int32_t kTargetRowLines = 2;
    /// Candidates shown at once, which is also what PageUp/PageDown moves by.
    constexpr std::size_t kCompletionPageSize = 6;
    /// Width of the tag name column.
    constexpr std::int32_t kTagColumns = 32;
    /// Width of the column carrying how many targets already carry a tag.
    constexpr std::int32_t kTagStatusColumns = 8;
    /// Tag rows PageUp and PageDown move by.
    constexpr std::int32_t kTagPageRows = 8;
    /**
     * @brief Suggested tags the list draws at once, matching the GTK tag editor.
     *
     * The cap is on the rows drawn, never on the vocabulary searched: a tag
     * ranked below it stays reachable by typing its name. Tags the draft
     * already marks are exempt, or marking one and then dropping the query
     * would hide the edit that was just made.
     */
    constexpr std::size_t kVisibleTagSuggestions = 50;

    bool isNumberRow(uimodel::TrackPropertiesFormRow const& row) noexcept
    {
      return row.editorKind == uimodel::TrackPropertiesFormEditorKind::Number;
    }

    /// Whether @p text parses under the field's own codec, which is what Save will use.
    bool parsesForRow(uimodel::TrackPropertiesFormRow const& row, std::string_view const text)
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

    std::string tagSearchKey(std::string_view const name)
    {
      auto keyRes = utility::makeUtf8CaselessKey(name);
      AO_INVARIANT(keyRes, "Validated tag text failed Unicode case folding: {}", keyRes.error().message);
      return std::move(*keyRes);
    }
  } // namespace

  TrackPropertiesEditor::TrackPropertiesEditor(i18n::MessageCatalog textCatalog,
                                               TrackEditorPreparation preparation,
                                               CompletionProvider completionProvider)
    : _textCatalog{std::move(textCatalog)}
    , _targets{std::move(preparation.targets)}
    , _baseline{std::move(preparation.baseline)}
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

    _tags.reserve(preparation.tagCounts.size() + preparation.tagSuggestions.size());

    for (auto& [tag, count] : preparation.tagCounts)
    {
      auto searchKey = tagSearchKey(tag);
      _tags.push_back(TagRow{
        .name = std::move(tag),
        .searchKey = std::move(searchKey),
        .originalCount = count,
        .intent = TagIntent::Preserve,
        .suggested = false,
      });
    }

    for (auto& tag : preparation.tagSuggestions)
    {
      auto searchKey = tagSearchKey(tag);
      _tags.push_back(TagRow{
        .name = std::move(tag),
        .searchKey = std::move(searchKey),
        .originalCount = 0,
        .intent = TagIntent::Preserve,
        .suggested = true,
      });
    }

    refreshVisibleTags();
  }

  bool TrackPropertiesEditor::isDirty() const noexcept
  {
    if (std::ranges::any_of(_metadataRows, [](auto const& row) { return row.intent != FieldIntent::Unchanged; }))
    {
      return true;
    }

    return std::ranges::any_of(_tags, [this](auto const& tag) { return isEffectiveTagEdit(tag); });
  }

  bool TrackPropertiesEditor::isEffectiveTagEdit(TagRow const& tag) const noexcept
  {
    // Adding a tag every target already carries, or removing one none of them
    // carries, writes nothing. The footer, Apply, and the patch all have to
    // agree about that, so they all ask here.
    return (tag.intent == TagIntent::AddToAll && tag.originalCount < _targets.size()) ||
           (tag.intent == TagIntent::RemoveFromAll && tag.originalCount > 0);
  }

  void TrackPropertiesEditor::setStatus(TrackEditorStatus const status, std::string diagnostic)
  {
    _status = status;
    _diagnostic = std::move(diagnostic);

    // A question about a draft is meaningless once the write it guarded is
    // already running or already refused.
    if (_status != TrackEditorStatus::Ready)
    {
      closeCompletion();
      _confirmingDiscard = false;
      _confirmingReload = false;
    }
  }

  TrackEditorRequest TrackPropertiesEditor::takeRequest() noexcept
  {
    return std::exchange(_request, TrackEditorRequest::None);
  }

  bool TrackPropertiesEditor::canApply() const noexcept
  {
    return _status == TrackEditorStatus::Ready && isDirty() &&
           std::ranges::none_of(_metadataRows, [](auto const& row) { return row.invalid; });
  }

  TrackEditorPatchSummary TrackPropertiesEditor::patchSummary() const noexcept
  {
    auto summary = TrackEditorPatchSummary{};

    for (auto const& row : _metadataRows)
    {
      if (row.intent != FieldIntent::Unchanged)
      {
        ++summary.fieldCount;

        if (row.intent == FieldIntent::ExplicitClear)
        {
          ++summary.clearCount;
        }
      }
    }

    for (auto const& tag : _tags)
    {
      if (!isEffectiveTagEdit(tag))
      {
        continue;
      }

      if (tag.intent == TagIntent::AddToAll)
      {
        ++summary.tagAddCount;
      }
      else
      {
        ++summary.tagRemoveCount;
      }
    }

    return summary;
  }

  rt::TrackPropertiesPatch TrackPropertiesEditor::buildPatch() const
  {
    auto patch = rt::TrackPropertiesPatch{};
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

    patch.metadata = form.buildPatch();

    for (auto const& tag : _tags)
    {
      if (!isEffectiveTagEdit(tag))
      {
        continue;
      }

      if (tag.intent == TagIntent::AddToAll)
      {
        patch.tagsToAdd.push_back(tag.name);
      }
      else
      {
        patch.tagsToRemove.push_back(tag.name);
      }
    }

    return patch;
  }

  bool TrackPropertiesEditor::handleEvent(ftxui::Event const& event)
  {
    // A write in flight cannot be steered, cancelled, or escaped from, so the
    // surface stays visible and inert until its terminal result arrives.
    if (_status == TrackEditorStatus::Submitting)
    {
      return true;
    }

    if (handleConfirmationEvent(event))
    {
      return true;
    }

    if (_status == TrackEditorStatus::Ready && !event.is_mouse())
    {
      _diagnostic.clear();
    }

    if (_tab == TrackEditorTab::Metadata && _optActiveCompletion)
    {
      if (handleCompletionEvent(event))
      {
        return true;
      }
    }

    // A query filters the tag list and is never part of the draft, so Escape
    // drops it before it says anything about the editor itself.
    if (_tab == TrackEditorTab::Tags && event == ftxui::Event::Escape && !_tagQuery.value().empty())
    {
      _tagQuery.reset("");
      refreshVisibleTags();
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
        _request = TrackEditorRequest::Close;
      }

      return true;
    }

    if (event == kApplyEvent)
    {
      if (canApply())
      {
        _request = TrackEditorRequest::Apply;
      }

      return true;
    }

    if (event == kReloadEvent)
    {
      // Reload re-reads every captured target, so a draft is genuinely lost.
      if (isDirty())
      {
        _confirmingReload = true;
      }
      else
      {
        _request = TrackEditorRequest::Reload;
      }

      return true;
    }

    if (event == ftxui::Event::Tab)
    {
      cycleTab(1);
      return true;
    }

    if (event == ftxui::Event::TabReverse)
    {
      cycleTab(-1);
      return true;
    }

    switch (_tab)
    {
      case TrackEditorTab::Metadata: handleMetadataEvent(event); break;
      case TrackEditorTab::Tags: handleTagsEvent(event); break;
      case TrackEditorTab::Properties:
      case TrackEditorTab::Tracks: handleReadonlyEvent(event); break;
    }

    return true;
  }

  bool TrackPropertiesEditor::handleConfirmationEvent(ftxui::Event const& event)
  {
    // A confirmation owns the keyboard until it is answered, so a stray key
    // cannot both dismiss the question and act on the form behind it.
    if (!_confirmingDiscard && !_confirmingReload)
    {
      return false;
    }

    if (event == ftxui::Event::Return)
    {
      _request = _confirmingReload ? TrackEditorRequest::Reload : TrackEditorRequest::Close;
    }
    else if (event != ftxui::Event::Escape)
    {
      return true;
    }

    _confirmingDiscard = false;
    _confirmingReload = false;
    return true;
  }

  void TrackPropertiesEditor::handleMetadataEvent(ftxui::Event const& event)
  {
    if (_metadataRows.empty() || _focusedMetadataRow >= _metadataRows.size())
    {
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

    auto& row = _metadataRows[_focusedMetadataRow];

    if (event == kClearEvent)
    {
      clearField(row);
      closeCompletion();
      return;
    }

    if (event == kRestoreEvent)
    {
      restoreField(row);
      closeCompletion();
      return;
    }

    if (event == kCompleteEvent)
    {
      maybeTriggerCompletion(row, true);
      return;
    }

    if (event == ftxui::Event::ArrowLeft)
    {
      row.input.moveLeft();
      return;
    }

    if (event == ftxui::Event::ArrowRight)
    {
      row.input.moveRight();
      return;
    }

    if (event == ftxui::Event::Home)
    {
      row.input.moveToBegin();
      return;
    }

    if (event == ftxui::Event::End)
    {
      row.input.moveToEnd();
      return;
    }

    if (event == ftxui::Event::Backspace)
    {
      if (row.input.backspace())
      {
        noteRowEdited(row);
        maybeTriggerCompletion(row, false);
      }

      return;
    }

    if (event == ftxui::Event::Delete)
    {
      if (row.input.deleteForward())
      {
        noteRowEdited(row);
        maybeTriggerCompletion(row, false);
      }

      return;
    }

    if (event.is_character() && row.input.insert(event.character()))
    {
      noteRowEdited(row);
      maybeTriggerCompletion(row, false);
    }
  }

  void TrackPropertiesEditor::handleTagsEvent(ftxui::Event const& event)
  {
    if (event == ftxui::Event::ArrowUp)
    {
      moveTagRow(-1);
      return;
    }

    if (event == ftxui::Event::ArrowDown)
    {
      moveTagRow(1);
      return;
    }

    if (event == ftxui::Event::PageUp)
    {
      moveTagRow(-kTagPageRows);
      return;
    }

    if (event == ftxui::Event::PageDown)
    {
      moveTagRow(kTagPageRows);
      return;
    }

    if (event == ftxui::Event::Return)
    {
      commitFocusedTag();
      return;
    }

    if (event == kRestoreEvent)
    {
      if (_focusedTagRow < _visibleTags.size())
      {
        // Restoring puts a suggestion back under the display cap that its own
        // intent had exempted it from, so the rows are derived again and the
        // selection follows the tag rather than snapping to the first row.
        auto const tagIndex = _visibleTags[_focusedTagRow];
        _tags[tagIndex].intent = TagIntent::Preserve;
        refreshVisibleTags();
        focusTag(tagIndex);
      }

      return;
    }

    // Everything the list does not claim is text. The query owns the keyboard
    // because typing a name is the only way to reach a tag the display cap
    // leaves out, and it is what names a tag the library does not have yet.
    if (event == ftxui::Event::ArrowLeft)
    {
      _tagQuery.moveLeft();
      return;
    }

    if (event == ftxui::Event::ArrowRight)
    {
      _tagQuery.moveRight();
      return;
    }

    if (event == ftxui::Event::Home)
    {
      _tagQuery.moveToBegin();
      return;
    }

    if (event == ftxui::Event::End)
    {
      _tagQuery.moveToEnd();
      return;
    }

    if (event == ftxui::Event::Backspace)
    {
      if (_tagQuery.backspace())
      {
        refreshVisibleTags();
      }

      return;
    }

    if (event == ftxui::Event::Delete)
    {
      if (_tagQuery.deleteForward())
      {
        refreshVisibleTags();
      }

      return;
    }

    if (event.is_character() && _tagQuery.insert(event.character()))
    {
      refreshVisibleTags();
    }
  }

  void TrackPropertiesEditor::handleReadonlyEvent(ftxui::Event const& event)
  {
    constexpr std::int32_t kPageRows = 8;

    if (event == ftxui::Event::ArrowUp)
    {
      scrollReadonly(-1);
      return;
    }

    if (event == ftxui::Event::ArrowDown)
    {
      scrollReadonly(1);
      return;
    }

    if (event == ftxui::Event::PageUp)
    {
      scrollReadonly(-kPageRows);
      return;
    }

    if (event == ftxui::Event::PageDown)
    {
      scrollReadonly(kPageRows);
    }
  }

  void TrackPropertiesEditor::cycleTab(std::int32_t const delta)
  {
    closeCompletion();
    _tagQuery.reset("");
    refreshVisibleTags();

    auto const tabs = availableTabs();
    auto const it = std::ranges::find(tabs, _tab);
    auto const current = static_cast<std::int32_t>(it != tabs.end() ? it - tabs.begin() : 0);
    auto const count = static_cast<std::int32_t>(tabs.size());
    auto const next = (((current + delta) % count) + count) % count;
    _tab = tabs[static_cast<std::size_t>(next)];
  }

  std::vector<TrackEditorTab> TrackPropertiesEditor::availableTabs() const
  {
    auto tabs = std::vector{TrackEditorTab::Metadata, TrackEditorTab::Tags, TrackEditorTab::Properties};

    if (hasTracksTab())
    {
      tabs.push_back(TrackEditorTab::Tracks);
    }

    return tabs;
  }

  void TrackPropertiesEditor::moveMetadataRow(std::int32_t const delta)
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

  void TrackPropertiesEditor::moveTagRow(std::int32_t const delta)
  {
    auto const rowCount = static_cast<std::int32_t>(_visibleTags.size()) + (_offersNewTag ? 1 : 0);

    if (rowCount == 0)
    {
      return;
    }

    auto const target = std::clamp(static_cast<std::int32_t>(_focusedTagRow) + delta, 0, rowCount - 1);
    _focusedTagRow = static_cast<std::size_t>(target);
  }

  void TrackPropertiesEditor::scrollReadonly(std::int32_t const delta)
  {
    if (_tab == TrackEditorTab::Tracks)
    {
      auto const count = static_cast<std::int32_t>(_targets.size());

      if (count == 0)
      {
        return;
      }

      auto const target = std::clamp(static_cast<std::int32_t>(_tracksRow) + delta, 0, count - 1);
      _tracksRow = static_cast<std::size_t>(target);
      return;
    }

    auto const count = static_cast<std::int32_t>(_spec.propertyRows.size());

    if (count == 0)
    {
      return;
    }

    auto const target = std::clamp(static_cast<std::int32_t>(_readonlyRow) + delta, 0, count - 1);
    _readonlyRow = static_cast<std::size_t>(target);
  }

  void TrackPropertiesEditor::clearField(MetadataRow& row)
  {
    row.input.reset("");
    row.intent = FieldIntent::ExplicitClear;
    revalidate(row);
  }

  void TrackPropertiesEditor::restoreField(MetadataRow& row)
  {
    row.intent = FieldIntent::Unchanged;
    row.invalid = false;
    row.input.reset(row.mixed ? "" : row.baselineText);
  }

  void TrackPropertiesEditor::noteRowEdited(MetadataRow& row)
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

  void TrackPropertiesEditor::revalidate(MetadataRow& row)
  {
    row.invalid = (row.intent != FieldIntent::Unchanged) && !parsesForRow(row.spec, row.input.value());
  }

  void TrackPropertiesEditor::maybeTriggerCompletion(MetadataRow const& row, bool const explicitRequest)
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

  void TrackPropertiesEditor::closeCompletion()
  {
    _optActiveCompletion.reset();
    _selectedCandidate = 0;
    _completionWindowStart = 0;
  }

  bool TrackPropertiesEditor::handleCompletionNavigation(ftxui::Event const& event, std::size_t const itemCount)
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

  bool TrackPropertiesEditor::handleCompletionEvent(ftxui::Event const& event)
  {
    if (_optActiveCompletion && handleCompletionNavigation(event, _optActiveCompletion->items.size()))
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

        if (auto const beforeVal = row.input.value(); row.input.replaceRange(
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
        event == ftxui::Event::End)
    {
      closeCompletion();
      return false;
    }

    if (event == kApplyEvent || event == kReloadEvent || event == kClearEvent || event == kRestoreEvent ||
        event == ftxui::Event::Tab || event == ftxui::Event::TabReverse)
    {
      closeCompletion();
      return false;
    }

    return false;
  }

  std::optional<std::string> TrackPropertiesEditor::normalizedTagQuery() const
  {
    auto normalizedRes = utility::normalizeUtf8Nfc(_tagQuery.value());

    if (!normalizedRes)
    {
      return std::nullopt;
    }

    return std::move(*normalizedRes);
  }

  void TrackPropertiesEditor::refreshVisibleTags()
  {
    _visibleTags.clear();
    _hiddenSuggestionCount = 0;
    _offersNewTag = false;

    // Compare normalized caseless keys, but preserve the stored spelling when
    // accepting an existing tag and the normalized spelling when creating one.
    auto const optNormalizedQuery = normalizedTagQuery();
    auto const query = tagSearchKey(_tagQuery.value());
    std::size_t shownSuggestions = 0;
    bool exactMatch = false;

    for (std::size_t index = 0; index < _tags.size(); ++index)
    {
      auto const& tag = _tags[index];
      auto const matchesExactly = !query.empty() && tag.searchKey == query;

      if (matchesExactly)
      {
        exactMatch = true;
      }

      if (!query.empty() && !tag.searchKey.contains(query))
      {
        continue;
      }

      // Draft tags and exact matches stay reachable regardless of frequency.
      if (tag.suggested && tag.intent == TagIntent::Preserve && !matchesExactly)
      {
        if (shownSuggestions == kVisibleTagSuggestions)
        {
          ++_hiddenSuggestionCount;
          continue;
        }

        ++shownSuggestions;
      }

      _visibleTags.push_back(index);
    }

    // The normalized form is what a submission would write, so a query that
    // normalizes to nothing names no tag worth offering.
    if (!query.empty() && !exactMatch)
    {
      _offersNewTag = optNormalizedQuery && !utility::trim(*optNormalizedQuery).empty();
    }

    _focusedTagRow = 0;
  }

  void TrackPropertiesEditor::focusTag(std::size_t const tagIndex)
  {
    auto const it = std::ranges::find(_visibleTags, tagIndex);
    _focusedTagRow = it == _visibleTags.end() ? 0 : static_cast<std::size_t>(std::distance(_visibleTags.begin(), it));
  }

  void TrackPropertiesEditor::cycleTagIntent(TagRow& tag) noexcept
  {
    // Adding a tag every target already carries writes nothing, and neither
    // does removing one none of them carries, so the cycle visits only the
    // intents that would reach storage. A mixed selection is the one case with
    // a real choice, and it is the one case with three stops.
    switch (auto const total = _targets.size(); tag.intent)
    {
      case TagIntent::Preserve:
        tag.intent = tag.originalCount == total ? TagIntent::RemoveFromAll : TagIntent::AddToAll;
        break;
      case TagIntent::AddToAll:
        tag.intent = tag.originalCount == 0 ? TagIntent::Preserve : TagIntent::RemoveFromAll;
        break;
      case TagIntent::RemoveFromAll: tag.intent = TagIntent::Preserve; break;
    }
  }

  void TrackPropertiesEditor::commitFocusedTag()
  {
    std::size_t tagIndex = 0;

    if (_focusedTagRow < _visibleTags.size())
    {
      tagIndex = _visibleTags[_focusedTagRow];
      cycleTagIntent(_tags[tagIndex]);
    }
    else if (_offersNewTag)
    {
      auto optNormalized = normalizedTagQuery();

      if (!optNormalized || utility::trim(*optNormalized).empty())
      {
        return;
      }

      // _tags stays partitioned into the selection's own tags and then the
      // library suggestions, so a created tag joins the first group rather
      // than being buried under every suggestion the library has.
      auto const firstSuggested = std::ranges::find(_tags, true, &TagRow::suggested);
      auto searchKey = tagSearchKey(*optNormalized);
      auto const inserted = _tags.insert(firstSuggested,
                                         TagRow{
                                           .name = std::move(*optNormalized),
                                           .searchKey = std::move(searchKey),
                                           .originalCount = 0,
                                           .intent = TagIntent::AddToAll,
                                           .suggested = false,
                                         });
      tagIndex = static_cast<std::size_t>(std::distance(_tags.begin(), inserted));
    }
    else
    {
      return;
    }

    // The query has done its job. Dropping it puts the tag back among its
    // neighbours so the change is read in context, and leaves the field ready
    // for the next name.
    _tagQuery.reset("");
    refreshVisibleTags();
    focusTag(tagIndex);
  }

  ftxui::Element TrackPropertiesEditor::renderTabStrip() const
  {
    using namespace ftxui;

    auto parts = Elements{};

    auto appendTab = [&](TrackEditorTab const tab, MessageId const id)
    {
      if (!parts.empty())
      {
        parts.push_back(style::mutedSeparator("│"));
      }

      auto labelPtr = text(" " + std::string{i18n::requiredText(_textCatalog, id)} + " ");

      if (tab != _tab)
      {
        parts.push_back(std::move(labelPtr) | style::muted());
        return;
      }

      // Reverse video pins no palette slot, so which page is open survives a
      // monochrome terminal and any theme.
      parts.push_back(std::move(labelPtr) | inverted | bold);
    };

    appendTab(TrackEditorTab::Metadata, MessageId::TuiEditorTabMetadata);
    appendTab(TrackEditorTab::Tags, MessageId::TuiEditorTabTags);
    appendTab(TrackEditorTab::Properties, MessageId::TuiEditorTabProperties);

    if (hasTracksTab())
    {
      appendTab(TrackEditorTab::Tracks, MessageId::TuiEditorTabTracks);
    }

    return hbox(std::move(parts));
  }

  ftxui::Element TrackPropertiesEditor::renderFieldValue(MetadataRow const& row, bool const focused) const
  {
    using namespace ftxui;

    if (row.intent == FieldIntent::ExplicitClear)
    {
      auto clearPtr =
        text(countedText(_textCatalog, MessageId::TuiEditorClearForAll, _targets.size())) | style::warning();
      return focused ? hbox({emptyInputCaret(), std::move(clearPtr)}) : std::move(clearPtr);
    }

    if (row.intent == FieldIntent::Unchanged && row.mixed && row.input.empty())
    {
      auto markerPtr =
        text(std::string{i18n::requiredText(_textCatalog, MessageId::TrackMultipleValues)}) | style::muted();
      return focused ? hbox({emptyInputCaret(), std::move(markerPtr)}) : std::move(markerPtr);
    }

    auto const& value = row.input.value();

    if (!focused)
    {
      return text(value);
    }

    auto const cursor = std::min(row.input.cursor(), value.size());
    auto const boundaryRes = utility::nextUtf8GraphemeBoundary(value, cursor);
    auto const caretEnd = boundaryRes ? *boundaryRes : cursor;

    return hbox({
             text(value.substr(0, cursor)),
             text(caretEnd > cursor ? value.substr(cursor, caretEnd - cursor) : std::string{" "}) | inverted | focus,
             text(caretEnd < value.size() ? value.substr(caretEnd) : std::string{}),
           }) |
           xframe;
  }

  ftxui::Element TrackPropertiesEditor::renderMetadataBody(std::int32_t const labelColumns) const
  {
    using namespace ftxui;

    auto rows = std::vector<SelectableListRow>{};
    rows.reserve(_metadataRows.size());
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
        renderFieldValue(row, rowFocused) | flex,
      };

      if (row.invalid)
      {
        cells.push_back(text(" "));
        cells.push_back(text(std::string{i18n::requiredText(_textCatalog, MessageId::TuiEditorInvalidNumber)}) |
                        style::danger());
      }

      auto rowElementPtr = hbox(std::move(cells));

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

          candidateElements.push_back(std::move(itemRowPtr));
        }

        auto popupBoxPtr = vbox(std::move(candidateElements)) | border | clear_under;
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
      std::move(rows), SelectableListOptions{.focusRow = focusLine, .horizontalScroll = false, .flex = true});
  }

  ftxui::Element TrackPropertiesEditor::renderTagCheckbox(TagRow const& tag) const
  {
    using namespace ftxui;

    // The box shows what every target would carry after a submission, so a
    // pending intent moves it now and colours it to say the move is pending.
    if (tag.intent == TagIntent::AddToAll)
    {
      return text("[x] ") | style::success();
    }

    if (tag.intent == TagIntent::RemoveFromAll)
    {
      return text("[ ] ") | style::danger();
    }

    if (tag.originalCount == 0)
    {
      return text("[ ] ");
    }

    return text(tag.originalCount == _targets.size() ? "[x] " : "[~] ");
  }

  ftxui::Element TrackPropertiesEditor::renderTagStatus(TagRow const& tag, std::size_t const total) const
  {
    using namespace ftxui;

    // The box shows where the tag is going, which leaves the fraction as the
    // only place it says where the tag started. A tag every target carries or
    // none does has no starting point the box does not already show, so only a
    // partly carried one spends a column on it.
    auto const partlyCarried = tag.originalCount > 0 && tag.originalCount < total;
    auto countText = partlyCarried ? std::format("{}/{}", tag.originalCount, total) : std::string{};

    if (tag.intent == TagIntent::Preserve)
    {
      return text(std::move(countText));
    }

    auto const verbId =
      tag.intent == TagIntent::AddToAll ? MessageId::TuiEditorTagsStateAdd : MessageId::TuiEditorTagsStateRemove;
    auto verbPtr = text(std::string{i18n::requiredText(_textCatalog, verbId)});

    return hbox({
      text(fitCellText(countText, kTagStatusColumns)),
      tag.intent == TagIntent::AddToAll ? std::move(verbPtr) | style::success() : std::move(verbPtr) | style::danger(),
    });
  }

  ftxui::Element TrackPropertiesEditor::renderTagsList() const
  {
    using namespace ftxui;

    auto const total = _targets.size();
    auto rows = std::vector<SelectableListRow>{};

    auto const marker = [](bool const focused)
    { return text(focused ? "> " : "  ") | (focused ? bold : style::muted()); };

    for (std::size_t row = 0; row < _visibleTags.size(); ++row)
    {
      auto const& tag = _tags[_visibleTags[row]];

      rows.push_back(SelectableListRow{
        .elementPtr = hbox({
          marker(_focusedTagRow == row),
          renderTagCheckbox(tag),
          text(fitCellText(tag.name, kTagColumns)),
          text(" "),
          renderTagStatus(tag, total),
        }),
      });
    }

    // Creation trails the matches so Enter lands on an existing tag whenever
    // the query found one, and only names a new tag when nothing matched.
    if (_offersNewTag)
    {
      auto const isFocused = _focusedTagRow == _visibleTags.size();
      auto newTagText =
        i18n::requiredFormat(_textCatalog, MessageId::TuiEditorTagsActionAddNew, {{"name", _tagQuery.value()}});

      rows.push_back(SelectableListRow{
        .elementPtr = hbox({
          marker(isFocused),
          text("    "),
          text(std::move(newTagText)) | bold,
        }),
      });
    }

    // Every row is exactly one line, so the row index is already the line
    // coordinate selectableList wants.
    return selectableList(std::move(rows),
                          SelectableListOptions{
                            .focusRow = static_cast<std::int32_t>(_focusedTagRow),
                            .horizontalScroll = false,
                            .flex = true,
                          });
  }

  ftxui::Element TrackPropertiesEditor::renderTagsBody() const
  {
    using namespace ftxui;

    // The query field is always live: the tab has no second mode to enter, so
    // a printable key is always a step towards naming a tag.
    auto const& query = _tagQuery.value();
    auto const cursor = std::min(_tagQuery.cursor(), query.size());
    auto const boundaryRes = utility::nextUtf8GraphemeBoundary(query, cursor);
    auto const caretEnd = boundaryRes ? *boundaryRes : cursor;

    auto queryCells = Elements{
      text(" "),
      text(query.substr(0, cursor)),
      text(caretEnd > cursor ? query.substr(cursor, caretEnd - cursor) : std::string{" "}) | inverted | focus,
      text(caretEnd < query.size() ? query.substr(caretEnd) : std::string{}),
    };

    if (query.empty())
    {
      // The caret occupies the placeholder's first cell, so the hint resumes
      // after it rather than sitting underneath it.
      queryCells.push_back(text(std::string{i18n::requiredText(_textCatalog, MessageId::TuiEditorTagsFilterHint)}) |
                           style::muted());
    }

    auto bodyElements = Elements{
      // Unbordered like every metadata field, so the caret is what says this
      // is an input and the rule below is the only thing dividing it from the
      // results it filters. A query wider than the modal scrolls to the caret
      // rather than shrinking every segment away from it.
      hbox(std::move(queryCells)) | xframe,
      separator(),
      renderTagsList(),
    };

    // A truncated list would otherwise claim the library holds nothing further.
    if (_hiddenSuggestionCount > 0)
    {
      bodyElements.push_back(text(i18n::requiredFormat(
                               _textCatalog, MessageId::TuiEditorTagsMoreHidden, {{"count", _hiddenSuggestionCount}})) |
                             style::muted());
    }

    return vbox(std::move(bodyElements));
  }

  ftxui::Element TrackPropertiesEditor::renderReadonlyBody() const
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

    return selectableList(
      std::move(rows), SelectableListOptions{.focusRow = static_cast<std::int32_t>(_readonlyRow), .flex = true});
  }

  ftxui::Element TrackPropertiesEditor::renderTargetBody() const
  {
    using namespace ftxui;

    auto rows = std::vector<SelectableListRow>{};
    rows.reserve(_targets.size());

    for (auto const& target : _targets)
    {
      rows.push_back(SelectableListRow{
        .elementPtr = vbox({
          text(target.title),
          hbox({text("  "), text(target.path) | style::muted()}),
        }),
      });
    }

    // The focus row is a line coordinate, and each target draws a title line
    // plus a path line, so the target index has to be scaled or the second
    // half of a large capture can never be scrolled into view.
    return selectableList(
      std::move(rows),
      SelectableListOptions{.focusRow = static_cast<std::int32_t>(_tracksRow) * kTargetRowLines, .flex = true});
  }

  ftxui::Element TrackPropertiesEditor::renderSaveSummary() const
  {
    using namespace ftxui;

    auto const summary = patchSummary();
    auto parts = Elements{};

    if (summary.fieldCount > 0)
    {
      parts.push_back(text(countedText(_textCatalog, MessageId::TuiEditorSummaryFields, summary.fieldCount)));
    }

    if (summary.clearCount > 0)
    {
      if (!parts.empty())
      {
        parts.push_back(style::mutedSeparator());
      }

      parts.push_back(text(countedText(_textCatalog, MessageId::TuiEditorSummaryClears, summary.clearCount)) |
                      style::warning());
    }

    if (summary.tagAddCount > 0)
    {
      if (!parts.empty())
      {
        parts.push_back(style::mutedSeparator());
      }

      parts.push_back(
        text(i18n::requiredFormat(_textCatalog, MessageId::TuiEditorSummaryTagsAdd, {{"count", summary.tagAddCount}})));
    }

    if (summary.tagRemoveCount > 0)
    {
      if (!parts.empty())
      {
        parts.push_back(style::mutedSeparator());
      }

      parts.push_back(text(i18n::requiredFormat(
        _textCatalog, MessageId::TuiEditorSummaryTagsRemove, {{"count", summary.tagRemoveCount}})));
    }

    if (_targets.size() > 1 && !parts.empty())
    {
      parts.push_back(style::mutedSeparator());
      parts.push_back(text(countedText(_textCatalog, MessageId::TuiEditorSummaryTargets, _targets.size())) |
                      style::muted());
    }

    return hbox(std::move(parts));
  }

  ftxui::Element TrackPropertiesEditor::renderFooter() const
  {
    using namespace ftxui;

    if (_confirmingReload)
    {
      return paragraph(std::string{i18n::requiredText(_textCatalog, MessageId::TuiEditorReloadPrompt)}) |
             style::warning();
    }

    if (_confirmingDiscard)
    {
      return paragraph(std::string{i18n::requiredText(_textCatalog, MessageId::TuiEditorDiscardPrompt)}) |
             style::warning();
    }

    if (_status == TrackEditorStatus::Submitting)
    {
      return text(std::string{i18n::requiredText(_textCatalog, MessageId::TuiEditorStatusSubmitting)}) | bold;
    }

    if (_status != TrackEditorStatus::Ready)
    {
      auto const message = _status == TrackEditorStatus::Stale
                             ? std::string{i18n::requiredText(_textCatalog, MessageId::TuiEditorStatusStale)}
                             : _diagnostic;
      return vbox({
        paragraph(message) | style::danger(),
        hbox({
          style::shortcutChip("Ctrl-R", i18n::requiredText(_textCatalog, MessageId::TuiEditorHintReload)),
          style::mutedSeparator(),
          style::shortcutChip("Esc", i18n::requiredText(_textCatalog, MessageId::TuiEditorHintClose)),
        }),
      });
    }

    auto makeRow = [](Elements chips) -> Element
    {
      if (chips.empty())
      {
        return text("");
      }

      auto withSeparators = Elements{};

      for (std::size_t i = 0; i < chips.size(); ++i)
      {
        if (i > 0)
        {
          withSeparators.push_back(style::mutedSeparator());
        }

        withSeparators.push_back(std::move(chips[i]));
      }

      return hbox(std::move(withSeparators));
    };

    auto row1Chips = Elements{};
    auto row2Chips = Elements{};

    if (_tab == TrackEditorTab::Metadata)
    {
      if (_optActiveCompletion)
      {
        row1Chips.push_back(
          style::shortcutChip("Up/Down", i18n::requiredText(_textCatalog, MessageId::TuiEditorHintSelect)));
        row1Chips.push_back(
          style::shortcutChip("Enter", i18n::requiredText(_textCatalog, MessageId::TuiEditorHintAccept)));
        // Escape closes the popup here and leaves the editor open, so naming
        // it "close" would promise the wrong exit.
        row2Chips.push_back(
          style::shortcutChip("Esc", i18n::requiredText(_textCatalog, MessageId::TuiEditorHintDismiss)));
      }
      else
      {
        row1Chips.push_back(
          style::shortcutChip("Ctrl-U", i18n::requiredText(_textCatalog, MessageId::TuiEditorHintClear)));
        row1Chips.push_back(
          style::shortcutChip("Ctrl-G", i18n::requiredText(_textCatalog, MessageId::TuiEditorHintRestore)));

        if (_focusedMetadataRow < _metadataRows.size() &&
            rt::supportsTrackFieldValueCompletion(_metadataRows[_focusedMetadataRow].spec.field))
        {
          row1Chips.push_back(
            style::shortcutChip("Ctrl-N", i18n::requiredText(_textCatalog, MessageId::TuiEditorHintComplete)));
        }

        row2Chips.push_back(style::shortcutChip("Tab", i18n::requiredText(_textCatalog, MessageId::TuiEditorHintPage)));
        row2Chips.push_back(
          style::shortcutChip("Esc", i18n::requiredText(_textCatalog, MessageId::TuiEditorHintClose)));
      }
    }
    else if (_tab == TrackEditorTab::Tags)
    {
      row1Chips.push_back(
        style::shortcutChip("Up/Down", i18n::requiredText(_textCatalog, MessageId::TuiEditorHintSelect)));
      // The trailing row creates a tag rather than cycling one, so naming the
      // key "toggle" there would describe something the row cannot do.
      row1Chips.push_back(style::shortcutChip(
        "Enter",
        i18n::requiredText(
          _textCatalog, isCreatingTag() ? MessageId::TuiEditorHintAdd : MessageId::TuiEditorHintToggle)));
      row1Chips.push_back(
        style::shortcutChip("Ctrl-G", i18n::requiredText(_textCatalog, MessageId::TuiEditorHintRestore)));
      row2Chips.push_back(style::shortcutChip("Tab", i18n::requiredText(_textCatalog, MessageId::TuiEditorHintPage)));
      row2Chips.push_back(style::shortcutChip("Esc", i18n::requiredText(_textCatalog, MessageId::TuiEditorHintClose)));
    }
    else
    {
      row2Chips.push_back(style::shortcutChip("Tab", i18n::requiredText(_textCatalog, MessageId::TuiEditorHintPage)));
      row2Chips.push_back(style::shortcutChip("Esc", i18n::requiredText(_textCatalog, MessageId::TuiEditorHintClose)));
    }

    auto applyPtr =
      style::shortcutChip("Ctrl-S", countedText(_textCatalog, MessageId::TuiEditorApplyAction, _targets.size()));
    row2Chips.push_back(canApply() ? std::move(applyPtr) : std::move(applyPtr) | dim);

    if (!_diagnostic.empty())
    {
      return vbox({
        paragraph(_diagnostic) | style::warning(),
        makeRow(std::move(row2Chips)),
      });
    }

    if (row1Chips.empty())
    {
      return makeRow(std::move(row2Chips));
    }

    return vbox({
      makeRow(std::move(row1Chips)),
      makeRow(std::move(row2Chips)),
    });
  }

  ftxui::Element TrackPropertiesEditor::render() const
  {
    using namespace ftxui;

    std::int32_t labelColumns = 0;

    for (auto const& row : _metadataRows)
    {
      labelColumns = std::max(labelColumns, cellWidth(row.spec.label));
    }

    labelColumns = std::min(labelColumns, kMaximumLabelColumns);

    auto const title = _targets.size() == 1
                         ? std::string{i18n::requiredText(_textCatalog, MessageId::TuiEditorTitle)}
                         : countedText(_textCatalog, MessageId::TuiEditorTitleMultiple, _targets.size());

    auto header = Elements{text(title) | bold};

    if (isDirty())
    {
      header.push_back(text(" "));
      header.push_back(renderSaveSummary());
    }
    else if (_targets.size() > 1)
    {
      header.push_back(text(" "));
      header.push_back(text(countedText(_textCatalog, MessageId::TuiEditorAppliesToAll, _targets.size())) |
                       style::muted());
    }

    auto bodyPtr = [&]
    {
      switch (_tab)
      {
        case TrackEditorTab::Tags: return renderTagsBody();
        case TrackEditorTab::Properties: return renderReadonlyBody();
        case TrackEditorTab::Tracks: return renderTargetBody();
        case TrackEditorTab::Metadata: break;
      }

      return renderMetadataBody(labelColumns);
    }();

    return vbox({
             hbox(std::move(header)),
             renderTabStrip(),
             separator(),
             std::move(bodyPtr) | flex,
             separator(),
             renderFooter(),
           }) |
           border;
  }

  ftxui::Element TrackPropertiesEditor::renderModal(std::int32_t const terminalColumns,
                                                    std::int32_t const terminalRows) const
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
} // namespace ao::tui
