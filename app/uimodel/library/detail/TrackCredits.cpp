// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/uimodel/library/detail/TrackCredits.h>

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/library/Credits.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/library/LibrarySnapshot.h>
#include <ao/rt/projection/TrackDetailSnapshot.h>
#include <ao/uimodel/library/presentation/TrackPresentationText.h>
#include <ao/utility/String.h>
#include <ao/utility/UnicodeText.h>

#include <algorithm>
#include <array>
#include <bitset>
#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::uimodel
{
  namespace
  {
    bool hasCanonicalCreditText(library::Credit const& entry)
    {
      if (entry.name.empty() || utility::trim(entry.name) != entry.name || utility::trim(entry.role) != entry.role)
      {
        return false;
      }

      auto const nameRes = utility::isUtf8Nfc(entry.name);
      auto const roleRes = utility::isUtf8Nfc(entry.role);
      return nameRes && *nameRes && roleRes && *roleRes;
    }

    bool hasValidCommonCreditSections(TrackCreditSections const& sections,
                                      std::bitset<library::kCreditKindCount> const kinds)
    {
      for (std::size_t i = 0; i < sections.size(); ++i)
      {
        auto const& section = sections[i];

        if (!kinds.test(i) || section.mixed)
        {
          continue;
        }

        if (!section.optValue ||
            std::ranges::any_of(
              *section.optValue,
              [i](auto const& entry)
              { return static_cast<std::size_t>(entry.kind) != i || !hasCanonicalCreditText(entry); }))
        {
          return false;
        }
      }

      return true;
    }
  } // namespace

  std::bitset<library::kCreditKindCount> trackCreditScope(library::CreditKind const kind)
  {
    auto kinds = std::bitset<library::kCreditKindCount>{};

    if (library::isValidCreditKind(kind))
    {
      kinds.set(static_cast<std::size_t>(kind));
    }

    return kinds;
  }

  std::bitset<library::kCreditKindCount> allTrackCreditKinds()
  {
    auto kinds = std::bitset<library::kCreditKindCount>{};
    kinds.set();
    return kinds;
  }

  rt::AggregateValue<std::vector<library::Credit>> scopedTrackCreditsBaseline(
    TrackCreditSections const& sections,
    std::bitset<library::kCreditKindCount> const kinds)
  {
    auto value = rt::AggregateValue<std::vector<library::Credit>>{};

    if (kinds.none())
    {
      return value;
    }

    auto entries = std::vector<library::Credit>{};
    bool missing = false;

    for (std::size_t i = 0; i < sections.size(); ++i)
    {
      if (!kinds.test(i))
      {
        continue;
      }

      auto const& section = sections[i];

      if (section.mixed)
      {
        return {.mixed = true};
      }

      if (!section.optValue)
      {
        missing = true;
        continue;
      }

      entries.append_range(*section.optValue);
    }

    if (!missing)
    {
      value.optValue = std::move(entries);
    }

    return value;
  }

  Result<TrackCreditSections> loadTrackCreditsEditorBaseline(rt::LibrarySnapshot const& snapshot,
                                                             std::span<TrackId const> const targetIds)
  {
    if (targetIds.empty())
    {
      return makeError(Error::Code::InvalidInput, "Credits editing requires at least one target");
    }

    auto sections = TrackCreditSections{};
    bool first = true;

    for (auto const trackId : targetIds)
    {
      auto optEntries = snapshot.trackCredits(trackId);

      if (!optEntries)
      {
        if (snapshot.containsTrack(trackId))
        {
          return makeError(Error::Code::CorruptData, "Credits editing target has unreadable credits");
        }

        return makeError(Error::Code::NotFound, "Credits editing target not found");
      }

      // Stored snapshot credits already satisfy the canonical text and kind contract.
      auto segments = std::array<std::vector<library::Credit>, library::kCreditKindCount>{};

      for (auto& entry : *optEntries)
      {
        segments[static_cast<std::size_t>(entry.kind)].push_back(std::move(entry));
      }

      for (std::size_t i = 0; i < sections.size(); ++i)
      {
        if (auto& section = sections[i]; first)
        {
          section.optValue = std::move(segments[i]);
        }
        else if (!section.mixed && section.optValue && *section.optValue != segments[i])
        {
          section.optValue.reset();
          section.mixed = true;
        }
      }

      first = false;
    }

    return sections;
  }

  std::string_view trackCreditKindLabel(i18n::MessageCatalog const& textCatalog, library::CreditKind const kind)
  {
    switch (kind)
    {
      case library::CreditKind::Conductor: return trackFieldLabel(textCatalog, rt::TrackField::Conductor);
      case library::CreditKind::Ensemble: return trackFieldLabel(textCatalog, rt::TrackField::Ensemble);
      case library::CreditKind::Soloist: return trackFieldLabel(textCatalog, rt::TrackField::Soloist);
      case library::CreditKind::Performer:
        return i18n::requiredText(textCatalog, i18n::MessageId::TrackCreditPerformer);
    }

    return {};
  }

  std::string trackCreditScopeLabel(i18n::MessageCatalog const& textCatalog,
                                    std::bitset<library::kCreditKindCount> const kinds)
  {
    if (kinds.all())
    {
      return std::string{i18n::requiredText(textCatalog, i18n::MessageId::TrackCreditsAllKinds)};
    }

    auto label = std::string{};

    for (std::size_t i = 0; i < library::kCreditKindCount; ++i)
    {
      if (kinds.test(i))
      {
        if (!label.empty())
        {
          label += "; ";
        }

        label += trackCreditKindLabel(textCatalog, static_cast<library::CreditKind>(i));
      }
    }

    return label;
  }

  std::string formatTrackCreditSectionSummary(i18n::MessageCatalog const& textCatalog,
                                              rt::AggregateValue<std::vector<library::Credit>> const& section)
  {
    if (section.mixed)
    {
      return std::string{i18n::requiredText(textCatalog, i18n::MessageId::TrackMultipleValues)};
    }

    if (!section.optValue || section.optValue->empty())
    {
      return {};
    }

    return formatTrackCreditSummary(textCatalog, section.optValue->front().name, section.optValue->size());
  }

  std::string formatTrackCreditSummary(i18n::MessageCatalog const& textCatalog,
                                       std::string_view const firstName,
                                       std::size_t const count)
  {
    if (count == 0)
    {
      return {};
    }

    if (count == 1)
    {
      return std::string{firstName};
    }

    return i18n::requiredFormat(
      textCatalog, i18n::MessageId::TrackCreditsSummary, {{"first", firstName}, {"count", count - 1}});
  }

  std::vector<TrackCreditDisplayRow> formatTrackCreditDisplayRows(i18n::MessageCatalog const& textCatalog,
                                                                  TrackCreditSections const& sections)
  {
    auto rows = std::vector<TrackCreditDisplayRow>{};

    for (std::size_t i = 0; i < sections.size(); ++i)
    {
      auto const kind = static_cast<library::CreditKind>(i);
      auto const& section = sections[i];

      if (auto const label = trackCreditKindLabel(textCatalog, kind); section.mixed)
      {
        rows.push_back({.kind = kind,
                        .kindLabel = std::string{label},
                        .name = std::string{i18n::requiredText(textCatalog, i18n::MessageId::TrackMultipleValues)},
                        .role = {},
                        .mixed = true});
      }
      else if (section.optValue)
      {
        for (auto const& entry : *section.optValue)
        {
          rows.push_back({.kind = kind, .kindLabel = std::string{label}, .name = entry.name, .role = entry.role});
        }
      }
    }

    return rows;
  }

  bool shouldShowTrackCredits(bool const metadataExpanded,
                              bool const showEmpty,
                              bool const hasTargets,
                              TrackCreditSections const& sections,
                              bool const editing)
  {
    return metadataExpanded && hasTargets &&
           (showEmpty || editing ||
            std::ranges::any_of(sections,
                                [](auto const& section)
                                { return section.mixed || (section.optValue && !section.optValue->empty()); }));
  }

  std::optional<rt::CreditReplacement> undoValueForClearedTrackCredits(
    TrackCreditSections const& sections,
    std::bitset<library::kCreditKindCount> const kinds)
  {
    if (!hasValidCommonCreditSections(sections, kinds))
    {
      return std::nullopt;
    }

    auto value = scopedTrackCreditsBaseline(sections, kinds);

    if (value.mixed || !value.optValue || value.optValue->empty())
    {
      return std::nullopt;
    }

    return rt::CreditReplacement{.kinds = kinds, .entries = std::move(*value.optValue)};
  }

  std::string formatTrackCreditValidationError(i18n::MessageCatalog const& textCatalog,
                                               TrackCreditValidationError const& error)
  {
    auto id = i18n::MessageId{};

    switch (error.reason)
    {
      case TrackCreditValidationReason::BlankName: id = i18n::MessageId::TrackCreditBlankName; break;
      case TrackCreditValidationReason::InvalidNameText: id = i18n::MessageId::TrackCreditInvalidNameText; break;
      case TrackCreditValidationReason::InvalidRoleText: id = i18n::MessageId::TrackCreditInvalidRoleText; break;
      case TrackCreditValidationReason::InvalidKind: id = i18n::MessageId::TrackCreditInvalidKind; break;
      default: return {};
    }

    return i18n::requiredFormat(textCatalog, id, {{"row", error.rowIndex + 1}});
  }

  Result<> TrackCreditsEditorModel::begin(TrackCreditSections const& baseline,
                                          std::bitset<library::kCreditKindCount> const kinds)
  {
    if (_editing || (kinds.count() != 1 && !kinds.all()))
    {
      return makeError(Error::Code::InvalidInput, "An inactive Credits editor requires one kind or all kinds");
    }

    if (!hasValidCommonCreditSections(baseline, kinds))
    {
      return makeError(Error::Code::InvalidInput, "Credits baseline must contain complete canonical kind sections");
    }

    _baseline = scopedTrackCreditsBaseline(baseline, kinds);
    _kinds = kinds;
    _entries = _baseline.optValue.value_or(std::vector<library::Credit>{});
    _editing = true;
    _replacement = false;
    _explicitClear = false;
    _optFocusedRow.reset();
    return {};
  }

  void TrackCreditsEditorModel::cancel()
  {
    _editing = false;
    _replacement = false;
    _explicitClear = false;
    _baseline = {};
    _kinds.reset();
    _entries.clear();
    _optFocusedRow.reset();
  }

  bool TrackCreditsEditorModel::isEditing() const noexcept
  {
    return _editing;
  }

  bool TrackCreditsEditorModel::isMixedReplacement() const noexcept
  {
    return _editing && _baseline.mixed;
  }

  bool TrackCreditsEditorModel::canEdit() const noexcept
  {
    return _editing && (!_baseline.mixed || _replacement);
  }

  std::bitset<library::kCreditKindCount> TrackCreditsEditorModel::scope() const noexcept
  {
    return _kinds;
  }

  std::vector<library::Credit> const& TrackCreditsEditorModel::entries() const noexcept
  {
    return _entries;
  }

  void TrackCreditsEditorModel::beginReplacement()
  {
    if (_editing)
    {
      _replacement = true;
    }
  }

  void TrackCreditsEditorModel::clearScope()
  {
    if (_editing)
    {
      _replacement = true;
      _explicitClear = true;
      _entries.clear();
      _optFocusedRow.reset();
    }
  }

  void TrackCreditsEditorModel::addEntry(library::CreditKind const kind)
  {
    if (!canEdit() || !library::isValidCreditKind(kind) || !_kinds.test(static_cast<std::size_t>(kind)))
    {
      return;
    }

    auto const position = std::ranges::find_if(_entries, [kind](auto const& entry) { return entry.kind > kind; });
    _optFocusedRow = static_cast<std::size_t>(position - _entries.begin());
    // Explicit Clear intent survives draft edits until this immutable session ends.
    _entries.insert(position, library::Credit{.kind = kind});
  }

  void TrackCreditsEditorModel::updateName(std::size_t const index, std::string name)
  {
    if (canEdit() && index < _entries.size())
    {
      _entries[index].name = std::move(name);
      _optFocusedRow = index;
    }
  }

  void TrackCreditsEditorModel::updateRole(std::size_t const index, std::string role)
  {
    if (canEdit() && index < _entries.size())
    {
      _entries[index].role = std::move(role);
      _optFocusedRow = index;
    }
  }

  void TrackCreditsEditorModel::deleteEntry(std::size_t const index)
  {
    if (canEdit() && index < _entries.size())
    {
      _entries.erase(_entries.begin() + static_cast<std::ptrdiff_t>(index));
      _optFocusedRow =
        _entries.empty() ? std::nullopt : std::optional<std::size_t>{std::min(index, _entries.size() - 1)};
    }
  }

  void TrackCreditsEditorModel::moveEntry(std::size_t const index, std::size_t const destinationIndex)
  {
    if (!canEdit() || index >= _entries.size() || destinationIndex >= _entries.size() ||
        _entries[index].kind != _entries[destinationIndex].kind)
    {
      return;
    }

    auto const first = _entries.begin() + static_cast<std::ptrdiff_t>(std::min(index, destinationIndex));
    auto const last = _entries.begin() + static_cast<std::ptrdiff_t>(std::max(index, destinationIndex)) + 1;
    std::rotate(first, index < destinationIndex ? first + 1 : last - 1, last);
    _optFocusedRow = destinationIndex;
  }

  void TrackCreditsEditorModel::changeKind(std::size_t const index, library::CreditKind const kind)
  {
    if (!canEdit() || !_kinds.all() || !library::isValidCreditKind(kind) || index >= _entries.size() ||
        _entries[index].kind == kind)
    {
      return;
    }

    auto entry = std::move(_entries[index]);
    _entries.erase(_entries.begin() + static_cast<std::ptrdiff_t>(index));
    entry.kind = kind;
    auto const position = std::ranges::find_if(_entries, [kind](auto const& other) { return other.kind > kind; });
    _optFocusedRow = static_cast<std::size_t>(position - _entries.begin());
    _entries.insert(position, std::move(entry));
  }

  void TrackCreditsEditorModel::focusRow(std::optional<std::size_t> const optIndex)
  {
    if (!optIndex || *optIndex < _entries.size())
    {
      _optFocusedRow = optIndex;
    }
  }

  std::optional<std::size_t> TrackCreditsEditorModel::focusedRow() const noexcept
  {
    return _optFocusedRow;
  }

  std::vector<TrackCreditValidationError> TrackCreditsEditorModel::validationErrors() const
  {
    auto errors = std::vector<TrackCreditValidationError>{};

    for (std::size_t i = 0; i < _entries.size(); ++i)
    {
      if (auto const& entry = _entries[i];
          !library::isValidCreditKind(entry.kind) || !_kinds.test(static_cast<std::size_t>(entry.kind)))
      {
        errors.push_back({.rowIndex = i, .reason = TrackCreditValidationReason::InvalidKind});
      }
      else if (auto const name = utility::trim(entry.name); name.empty())
      {
        errors.push_back({.rowIndex = i, .reason = TrackCreditValidationReason::BlankName});
      }
      else if (!utility::validateUtf8(name))
      {
        errors.push_back({.rowIndex = i, .reason = TrackCreditValidationReason::InvalidNameText});
      }
      else if (!utility::validateUtf8(utility::trim(entry.role)))
      {
        errors.push_back({.rowIndex = i, .reason = TrackCreditValidationReason::InvalidRoleText});
      }
    }

    return errors;
  }

  bool TrackCreditsEditorModel::canCommit() const
  {
    return canEdit() && (!_baseline.mixed || !_entries.empty() || _explicitClear) && validationErrors().empty();
  }

  Result<rt::MetadataPatch> TrackCreditsEditorModel::buildCommitPatch() const
  {
    if (!canEdit() || (_baseline.mixed && _entries.empty() && !_explicitClear))
    {
      return makeError(Error::Code::InvalidInput, "Explicit replacement or clear intent is required for mixed Credits");
    }

    auto entriesRes = library::normalizeCredits(_entries);

    if (!entriesRes)
    {
      return std::unexpected{entriesRes.error()};
    }

    auto patch = rt::MetadataPatch{};

    if (_baseline.mixed || !_baseline.optValue || *_baseline.optValue != *entriesRes)
    {
      patch.optCredits = rt::CreditReplacement{.kinds = _kinds, .entries = std::move(*entriesRes)};
    }

    return patch;
  }
} // namespace ao::uimodel
