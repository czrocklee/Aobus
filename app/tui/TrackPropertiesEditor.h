// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "MouseBindings.h"
#include "TrackMetadataEditor.h"
#include "TrackTagEditor.h"
#include <ao/CoreIds.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/TrackMutation.h>
#include <ao/uimodel/library/property/TrackPropertiesFormModel.h>

#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/screen/box.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
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
    using CompletionProvider = TrackMetadataEditor::CompletionProvider;

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
    void handleMouse(ftxui::Mouse const& mouse);
    void handleTargetEvent(ftxui::Event const& event);
    void scrollTargets(std::int32_t delta);
    void selectTab(TrackEditorTab tab);
    void cycleTab(std::int32_t delta);
    std::vector<TrackEditorTab> availableTabs() const;
    bool tryHandleConfirmationEvent(ftxui::Event const& event);
    bool hasTracksTab() const noexcept { return _targets.size() > 1; }
    ftxui::Element renderTabStrip() const;
    ftxui::Element renderTargetBody() const;
    ftxui::Element renderFooter() const;
    ftxui::Element renderSaveSummary() const;

    i18n::MessageCatalog _textCatalog;
    std::vector<TrackEditorTarget> _targets{};
    TrackMetadataEditor _metadataEditor;
    TrackTagEditor _tagEditor;
    TrackEditorTab _tab = TrackEditorTab::Metadata;
    std::size_t _tracksRow = 0;
    mutable ftxui::Box _targetViewport = kEmptyMouseBox;
    TrackEditorStatus _status = TrackEditorStatus::Ready;
    TrackEditorRequest _request = TrackEditorRequest::None;
    std::string _diagnostic{};
    bool _confirmingDiscard = false;
    bool _confirmingReload = false;
    mutable bool _mouseReady = false;
    mutable MouseBindings _mouseBindings;
    mutable std::vector<ftxui::Box> _tabBoxes;
    mutable ftxui::Box _bodyBox = kEmptyMouseBox;
    mutable TrackEditorTab _renderedTab = TrackEditorTab::Metadata;
  };
} // namespace ao::tui
