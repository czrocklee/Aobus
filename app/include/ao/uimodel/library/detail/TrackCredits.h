// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/Credits.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/projection/TrackDetailSnapshot.h>

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ao::i18n
{
  class MessageCatalog;
}
namespace ao::rt
{
  class LibrarySnapshot;
}

namespace ao::uimodel
{
  // Common sections contain canonical (trimmed NFC) text and entries matching their section kind.
  // Snapshot loading and accepted editor patches guarantee this contract. Public callers constructing
  // sections must do the same; editor admission rejects malformed or noncanonical common sections.
  using TrackCreditSections = std::array<rt::AggregateValue<std::vector<library::Credit>>, library::kCreditKindCount>;

  std::bitset<library::kCreditKindCount> trackCreditScope(library::CreditKind kind);
  std::bitset<library::kCreditKindCount> allTrackCreditKinds();
  rt::AggregateValue<std::vector<library::Credit>> scopedTrackCreditsBaseline(
    TrackCreditSections const& sections,
    std::bitset<library::kCreditKindCount> kinds);

  // Capture binding from this same snapshot, then release it before UI work or submission.
  Result<TrackCreditSections> loadTrackCreditsEditorBaseline(rt::LibrarySnapshot const& snapshot,
                                                             std::span<TrackId const> targetIds);

  struct TrackCreditDisplayRow final
  {
    library::CreditKind kind = library::CreditKind::Performer;
    std::string kindLabel;
    std::string name;
    std::string role;
    bool mixed = false;
  };

  std::string_view trackCreditKindLabel(i18n::MessageCatalog const& textCatalog, library::CreditKind kind);
  // An editor scope is one kind or all kinds; an empty scope has no label.
  std::string trackCreditScopeLabel(i18n::MessageCatalog const& textCatalog,
                                    std::bitset<library::kCreditKindCount> kinds);
  std::string formatTrackCreditSectionSummary(i18n::MessageCatalog const& textCatalog,
                                              rt::AggregateValue<std::vector<library::Credit>> const& section);
  std::string formatTrackCreditSummary(i18n::MessageCatalog const& textCatalog,
                                       std::string_view firstName,
                                       std::size_t count);
  // A credit's name with its optional role; an absent role yields the name alone.
  std::string formatTrackCreditNameRole(i18n::MessageCatalog const& textCatalog,
                                        std::string_view name,
                                        std::string_view role);
  std::vector<TrackCreditDisplayRow> formatTrackCreditDisplayRows(i18n::MessageCatalog const& textCatalog,
                                                                  TrackCreditSections const& sections);
  bool shouldShowTrackCredits(bool metadataExpanded,
                              bool showEmpty,
                              bool hasTargets,
                              TrackCreditSections const& sections,
                              bool editing);
  // Whether a frontend offers a kind's one-kind entry point. Conductor,
  // Ensemble, Soloist, and Performer always have one; every other kind has
  // one only with credits on some target or while show-empty is active, and
  // stays editable through the all-kind editor.
  bool shouldShowTrackCreditKind(library::CreditKind kind,
                                 rt::AggregateValue<std::vector<library::Credit>> const& section,
                                 bool showEmpty);
  std::optional<rt::CreditReplacement> undoValueForClearedTrackCredits(TrackCreditSections const& sections,
                                                                       std::bitset<library::kCreditKindCount> kinds);
  enum class TrackCreditValidationReason : std::uint8_t
  {
    BlankName,
    InvalidNameText,
    InvalidRoleText,
    InvalidKind,
  };

  struct TrackCreditValidationError final
  {
    std::size_t rowIndex = 0;
    TrackCreditValidationReason reason = TrackCreditValidationReason::BlankName;

    bool operator==(TrackCreditValidationError const&) const = default;
  };

  // rowIndex is zero-based model state; the localized message displays a one-based row number.
  std::string formatTrackCreditValidationError(i18n::MessageCatalog const& textCatalog,
                                               TrackCreditValidationError const& error);

  enum class TrackCreditRowAction : std::uint8_t
  {
    MoveUp,
    MoveDown,
    Delete,
  };

  // Accessible name of one row's command, so repeated per-row commands stay
  // distinguishable. rowIndex is zero-based; the message shows a one-based row.
  std::string formatTrackCreditRowActionLabel(i18n::MessageCatalog const& textCatalog,
                                              TrackCreditRowAction action,
                                              std::size_t rowIndex);

  // One fixed scope per active session. Mixed scopes never seed a partial/first-target draft.
  // This model has no database side effects; its owner retains authoring binding and stale/busy policy.
  class TrackCreditsEditorModel final
  {
  public:
    Result<> begin(TrackCreditSections const& baseline, std::bitset<library::kCreditKindCount> kinds);
    void cancel();
    bool isEditing() const noexcept;
    // Whether discarding this session would lose user intent: a common draft
    // that differs from its baseline, or mixed replacement rows or Clear.
    // Opening, browsing, or arming Replace alone loses nothing.
    bool hasDraftChanges() const;
    bool isMixedReplacement() const noexcept;
    bool canEdit() const noexcept;
    std::bitset<library::kCreditKindCount> scope() const noexcept;
    std::vector<library::Credit> const& entries() const noexcept;
    // Kind for an Add without an explicit choice: a single-kind scope's kind,
    // otherwise Performer, the open-ended kind.
    library::CreditKind defaultAddKind() const noexcept;
    void beginReplacement();
    void clearScope();
    void addEntry(library::CreditKind kind);
    void updateName(std::size_t index, std::string name);
    void updateRole(std::size_t index, std::string role);
    void deleteEntry(std::size_t index);
    void moveEntry(std::size_t index, std::size_t destinationIndex);
    void changeKind(std::size_t index, library::CreditKind kind);
    void focusRow(std::optional<std::size_t> optIndex);
    std::optional<std::size_t> focusedRow() const noexcept;
    std::vector<TrackCreditValidationError> validationErrors() const;
    bool canCommit() const;
    Result<rt::MetadataPatch> buildCommitPatch() const;

  private:
    bool _editing = false;
    bool _replacement = false;
    bool _explicitClear = false;
    std::bitset<library::kCreditKindCount> _kinds;
    rt::AggregateValue<std::vector<library::Credit>> _baseline;
    std::vector<library::Credit> _entries;
    std::optional<std::size_t> _optFocusedRow;
  };
} // namespace ao::uimodel
