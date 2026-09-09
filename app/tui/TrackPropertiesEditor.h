// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "TuiTextFieldModel.h"
#include <ao/CoreIds.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/completion/CompletionResult.h>
#include <ao/uimodel/library/property/TrackPropertiesFormModel.h>
#include <ao/uimodel/library/property/TrackPropertiesFormSpec.h>

#include <ftxui/component/event.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ftxui
{
  class Node;
  using Element = std::shared_ptr<Node>;
} // namespace ftxui

namespace ao::tui
{
  /**
   * @brief One captured edit target, named well enough to review before saving.
   *
   * The identity is copied out of the preparing snapshot so the editor holds no
   * library row, borrowed field value, or live selection span for as long as it
   * is open.
   */
  struct TrackEditorTarget final
  {
    TrackId id = kInvalidTrackId;
    std::string title{};
    std::string path{};
  };

  /// Everything one coherent read produced for an editor.
  struct TrackEditorPreparation final
  {
    std::vector<TrackEditorTarget> targets{};
    /// The aggregate baseline: common values, mixed markers, and read-only rows.
    uimodel::TrackPropertiesFormModel baseline;
    /// Per-tag counts on the captured tracks: tag and how many tracks carry it.
    std::vector<std::pair<std::string, std::size_t>> tagCounts{};
    /// Tags the library knows that no captured track carries, most frequent first.
    std::vector<std::string> tagSuggestions{};
  };

  enum class TrackEditorTab : std::uint8_t
  {
    Metadata,
    Tags,
    Properties,
    Tracks,
  };

  /// What the editor is currently allowed to do about writing.
  enum class TrackEditorStatus : std::uint8_t
  {
    /// A valid draft can be submitted through the retained session.
    Ready,
    /// A submission is in flight; repeated Apply and close are consumed.
    Submitting,
    /// The session was invalidated; the draft survives but cannot be written.
    Stale,
    /// A submission failed; the draft survives and a coherent reload must precede another attempt.
    Failed,
  };

  /// What the editor asked its owner for, taken exactly once.
  enum class TrackEditorRequest : std::uint8_t
  {
    None,
    Close,
    Apply,
    Reload,
  };

  /// What a submission would write, in the terms the footer reports it.
  struct TrackEditorPatchSummary final
  {
    std::size_t fieldCount = 0;
    std::size_t clearCount = 0;
    std::size_t tagAddCount = 0;
    std::size_t tagRemoveCount = 0;

    bool operator==(TrackEditorPatchSummary const&) const = default;
  };

  /**
   * @brief The centered modal track properties and tags editor for one to many targets.
   */
  class TrackPropertiesEditor final
  {
  public:
    using CompletionProvider =
      std::function<std::optional<rt::CompletionResult>(rt::TrackField, std::string_view, std::size_t)>;

    TrackPropertiesEditor(i18n::MessageCatalog textCatalog,
                          TrackEditorPreparation preparation,
                          CompletionProvider completionProvider = {});

    std::size_t targetCount() const noexcept { return _targets.size(); }
    /// The captured targets, in the order they will be written.
    std::vector<TrackEditorTarget> const& targets() const noexcept { return _targets; }
    TrackEditorTab tab() const noexcept { return _tab; }
    /// Whether any field or tag carries intent to write.
    bool isDirty() const noexcept;
    /// Whether a dirty Escape or reload is waiting for the user to confirm losing the draft.
    bool isConfirmingDiscard() const noexcept { return _confirmingDiscard; }
    bool isConfirmingReload() const noexcept { return _confirmingReload; }

    TrackEditorStatus status() const noexcept { return _status; }
    std::string const& diagnostic() const noexcept { return _diagnostic; }
    /// Moves to @p status, replacing any diagnostic the previous status displayed.
    void setStatus(TrackEditorStatus status, std::string diagnostic = {});

    /// Consumes the pending owner request; a second call reports None.
    TrackEditorRequest takeRequest() noexcept;

    /// Whether Apply would be accepted right now.
    bool canApply() const noexcept;
    /// What the included fields and tag edits would write, for the footer's save summary.
    TrackEditorPatchSummary patchSummary() const noexcept;
    /// The combined metadata-and-tags patch.
    rt::TrackPropertiesPatch buildPatch() const;

    /// Consumes @p event; an active editor answers for every key the terminal delivers.
    bool tryHandleEvent(ftxui::Event const& event);
    ftxui::Element render() const;
    ftxui::Element renderModal(std::int32_t terminalColumns, std::int32_t terminalRows) const;

  private:
    enum class FieldIntent : std::uint8_t
    {
      Unchanged,
      Replacement,
      ExplicitClear,
    };

    struct MetadataRow final
    {
      uimodel::TrackPropertiesFormRow spec{};
      TuiTextFieldModel input{};
      /// The aggregate value this row falls back to; empty for a mixed field.
      std::string baselineText{};
      bool mixed = false;
      FieldIntent intent = FieldIntent::Unchanged;
      bool invalid = false;

      bool isIncluded() const noexcept { return intent != FieldIntent::Unchanged; }
    };

    enum class TagIntent : std::uint8_t
    {
      Preserve,
      AddToAll,
      RemoveFromAll,
    };

    struct TagRow final
    {
      std::string name{};
      std::string searchKey{};
      std::size_t originalCount = 0;
      TagIntent intent = TagIntent::Preserve;
      bool suggested = false;
    };

    void handleMetadataEvent(ftxui::Event const& event);
    void handleTagsEvent(ftxui::Event const& event);
    void handleReadonlyEvent(ftxui::Event const& event);

    void cycleTab(std::int32_t delta);
    std::vector<TrackEditorTab> availableTabs() const;

    void moveMetadataRow(std::int32_t delta);
    void moveTagRow(std::int32_t delta);
    void scrollReadonly(std::int32_t delta);

    void noteRowEdited(MetadataRow& row);
    void clearField(MetadataRow& row);
    void restoreField(MetadataRow& row);
    void revalidate(MetadataRow& row);

    void maybeTriggerCompletion(MetadataRow const& row, bool explicitRequest);
    void closeCompletion();
    bool tryHandleCompletionEvent(ftxui::Event const& event);
    bool tryHandleCompletionNavigation(ftxui::Event const& event, std::size_t itemCount);

    /// The query in the form a tag name is stored in, or nothing when it will not normalize.
    std::optional<std::string> normalizedTagQuery() const;
    /// Rebuilds the rows the query admits and parks the selection on the first of them.
    void refreshVisibleTags();
    /// Acts on the selected row: cycles a tag's intent, or creates the tag the query names.
    void commitFocusedTag();
    /// Advances @p tag through the intents that would actually write something.
    void cycleTagIntent(TagRow& tag) noexcept;
    /// Whether the selected tag row is the offer to create the tag the query names.
    bool isCreatingTag() const noexcept { return _offersNewTag && _focusedTagRow >= _visibleTags.size(); }
    /// Selects the row showing @p tagIndex, falling back to the first when the query hides it.
    void focusTag(std::size_t tagIndex);
    /// Whether @p tag's intent would actually change any captured track.
    bool isEffectiveTagEdit(TagRow const& tag) const noexcept;

    /// Answers a pending confirmation prompt; reports whether one consumed @p event.
    bool tryHandleConfirmationEvent(ftxui::Event const& event);

    bool hasTracksTab() const noexcept { return _targets.size() > 1; }

    ftxui::Element renderTabStrip() const;
    ftxui::Element renderMetadataBody(std::int32_t labelColumns) const;
    ftxui::Element renderTagsBody() const;
    ftxui::Element renderTagsList() const;
    ftxui::Element renderTagCheckbox(TagRow const& tag) const;
    ftxui::Element renderTagStatus(TagRow const& tag, std::size_t total) const;
    ftxui::Element renderReadonlyBody() const;
    ftxui::Element renderTargetBody() const;
    ftxui::Element renderFooter() const;
    ftxui::Element renderFieldValue(MetadataRow const& row, bool focused) const;
    ftxui::Element renderSaveSummary() const;

    i18n::MessageCatalog _textCatalog;
    std::vector<TrackEditorTarget> _targets{};
    uimodel::TrackPropertiesFormModel _baseline;
    uimodel::TrackPropertiesFormSpec _spec{};
    std::vector<MetadataRow> _metadataRows{};
    std::vector<TagRow> _tags{};
    CompletionProvider _completionProvider{};

    TrackEditorTab _tab = TrackEditorTab::Metadata;
    std::size_t _focusedMetadataRow = 0;
    /// Selection within the visible tag rows; _visibleTags.size() addresses the new-tag row.
    std::size_t _focusedTagRow = 0;
    std::size_t _readonlyRow = 0;
    std::size_t _tracksRow = 0;

    /// Filters the tag list and names the tag a submission would create; never part of the draft.
    TuiTextFieldModel _tagQuery{};
    /// Rows the query admits, as indices into _tags; the new-tag row trails them.
    std::vector<std::size_t> _visibleTags{};
    /// Suggested rows the display cap left out, so the list can own up to them.
    std::size_t _hiddenSuggestionCount = 0;
    /// Whether the query names a tag no row carries, offering creation on the trailing row.
    bool _offersNewTag = false;

    std::optional<rt::CompletionResult> _optActiveCompletion{};
    std::size_t _selectedCandidate = 0;
    std::size_t _completionWindowStart = 0;

    TrackEditorStatus _status = TrackEditorStatus::Ready;
    TrackEditorRequest _request = TrackEditorRequest::None;
    std::string _diagnostic{};
    bool _confirmingDiscard = false;
    bool _confirmingReload = false;
  };
} // namespace ao::tui
