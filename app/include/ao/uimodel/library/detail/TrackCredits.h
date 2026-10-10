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
  std::string trackCreditScopeLabel(i18n::MessageCatalog const& textCatalog,
                                    std::bitset<library::kCreditKindCount> kinds);
  std::string formatTrackCreditSectionSummary(i18n::MessageCatalog const& textCatalog,
                                              rt::AggregateValue<std::vector<library::Credit>> const& section);
  std::string formatTrackCreditSummary(i18n::MessageCatalog const& textCatalog,
                                       std::string_view firstName,
                                       std::size_t count);
  std::vector<TrackCreditDisplayRow> formatTrackCreditDisplayRows(i18n::MessageCatalog const& textCatalog,
                                                                  TrackCreditSections const& sections);
  bool shouldShowTrackCredits(bool metadataExpanded,
                              bool showEmpty,
                              bool hasTargets,
                              TrackCreditSections const& sections,
                              bool editing);
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
  };

  // rowIndex is zero-based model state; the localized message displays a one-based row number.
  std::string formatTrackCreditValidationError(i18n::MessageCatalog const& textCatalog,
                                               TrackCreditValidationError const& error);

  // One fixed scope per active session. Mixed scopes never seed a partial/first-target draft.
  // This model has no database side effects; its owner retains authoring binding and stale/busy policy.
  class TrackCreditsEditorModel final
  {
  public:
    Result<> begin(TrackCreditSections const& baseline, std::bitset<library::kCreditKindCount> kinds);
    void cancel();
    bool isEditing() const noexcept;
    bool isMixedReplacement() const noexcept;
    bool canEdit() const noexcept;
    std::bitset<library::kCreditKindCount> scope() const noexcept;
    std::vector<library::Credit> const& entries() const noexcept;
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
