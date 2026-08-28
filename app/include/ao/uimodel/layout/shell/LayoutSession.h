// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/uimodel/layout/component/LayoutComponentState.h>
#include <ao/uimodel/layout/component/LayoutSurface.h>
#include <ao/uimodel/layout/document/LayoutDocument.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace ao::uimodel
{
  class LayoutComponentStateStore;

  using LayoutNodeMovedFn =
    std::function<void(std::string const& nodeId, std::int32_t xPosition, std::int32_t yPosition)>;

  struct LayoutPresetSelection final
  {
    std::string presetId;
    bool usedFallback = false;
  };

  struct PanelSizePromotion final
  {
    LayoutDocument layout;
    LayoutComponentStateDocument componentState;
  };

  /**
   * @brief Immutable inputs captured for one candidate layout build.
   *
   * The state document is owned rather than borrowed. A rejected candidate can
   * therefore tear down after its caller's working copy changes, while every
   * component binding still compares against the generation it was built for.
   */
  class LayoutBuildSnapshot final
  {
  public:
    std::string_view presetId() const noexcept { return _componentState.preset; }
    LayoutComponentStateDocument const& componentState() const noexcept { return _componentState; }
    std::uint64_t generation() const noexcept { return _generation; }
    bool isEditMode() const noexcept { return _editMode; }
    LayoutNodeMovedFn const& onNodeMoved() const noexcept { return _onNodeMoved; }

  private:
    friend class LayoutSession;

    LayoutBuildSnapshot(LayoutComponentStateDocument componentState,
                        std::uint64_t generation,
                        bool editMode,
                        LayoutNodeMovedFn onNodeMoved);

    LayoutComponentStateDocument _componentState;
    std::uint64_t _generation = 0;
    bool _editMode = false;
    LayoutNodeMovedFn _onNodeMoved;
  };

  class LayoutSession;

  /**
   * @brief Generation-fenced state access retained by one stateful component.
   */
  class ComponentStateBinding final
  {
  public:
    std::optional<LayoutComponentStateEntry> const& restored() const noexcept { return _optRestored; }
    bool canWrite() const noexcept;
    void write(std::map<std::string, LayoutValue, std::less<>> state);

  private:
    friend class LayoutSession;

    ComponentStateBinding(LayoutSession& session,
                          LayoutBuildSnapshot const& snapshot,
                          LayoutSurface surface,
                          LayoutNode const& node,
                          std::string_view type);

    LayoutSession* _session = nullptr;
    std::string _componentId;
    std::string _type;
    std::string _presetId;
    std::string _baselineHash;
    std::uint64_t _generation = 0;
    bool _persistable = false;
    std::optional<LayoutComponentStateEntry> _optRestored;
  };

  /**
   * @brief Mutable layout document, component state, and generation owned by one shell session.
   */
  class LayoutSession final
  {
  public:
    static constexpr std::string_view kDefaultPresetId = "classic";

    explicit LayoutSession(LayoutComponentStateStore* componentStateStore = nullptr);

    static LayoutPresetSelection selectPreset(std::string_view requestedPresetId,
                                              std::span<std::string_view const> supportedPresetIds,
                                              std::string_view fallbackPresetId = kDefaultPresetId);
    static std::string activeOrDefaultPresetId(std::string_view activePresetId);
    static LayoutComponentStateDocument emptyComponentState(std::string_view presetId);

    std::string_view presetId() const noexcept { return _componentState.preset; }
    LayoutDocument const& layout() const noexcept { return _layout; }
    LayoutComponentStateDocument const& componentState() const noexcept { return _componentState; }
    std::uint64_t generation() const noexcept { return _generation; }
    bool isEditMode() const noexcept { return _editMode; }

    std::optional<LayoutBuildSnapshot> buildSnapshot() const;
    std::optional<LayoutBuildSnapshot> buildSnapshot(LayoutComponentStateDocument const& componentState,
                                                     bool editMode,
                                                     LayoutNodeMovedFn onNodeMoved = {}) const;

    ComponentStateBinding stateFor(LayoutBuildSnapshot const& snapshot,
                                   LayoutSurface surface,
                                   LayoutNode const& node,
                                   std::string_view type);

    void advanceGeneration(std::uint64_t generation);
    void apply(LayoutDocument layout, LayoutComponentStateDocument componentState, std::uint64_t generation);
    void setEditMode(bool editMode, LayoutNodeMovedFn onNodeMoved = {});

    std::optional<PanelSizePromotion> preparePanelSizePromotion() const;

  private:
    friend class ComponentStateBinding;

    LayoutDocument _layout;
    LayoutComponentStateDocument _componentState;
    LayoutComponentStateStore* _componentStateStore = nullptr;
    std::uint64_t _generation = 1;
    bool _editMode = false;
    LayoutNodeMovedFn _onNodeMoved;
  };
} // namespace ao::uimodel
