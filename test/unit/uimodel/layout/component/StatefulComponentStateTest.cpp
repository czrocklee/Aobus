// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors
#include <ao/uimodel/layout/component/StatefulComponentState.h>

#include <ao/uimodel/layout/component/LayoutComponentState.h>
#include <ao/uimodel/layout/component/LayoutComponentStateStore.h>
#include <ao/uimodel/layout/component/LayoutSurface.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/layout/shell/LayoutBuildStateView.h>
#include <ao/uimodel/layout/shell/LayoutRuntimeState.h>

#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::uimodel::test
{
  namespace
  {
    /// Records every save so tests can assert on both the count and the persisted payload.
    class RecordingStateStore final : public LayoutComponentStateStore
    {
    public:
      std::optional<LayoutComponentStateDocument> load(std::string_view /*presetId*/) const override { return {}; }

      void save(std::string_view const presetId, LayoutComponentStateDocument const& doc) override
      {
        _saved.emplace_back(std::string{presetId}, doc);
      }

      bool prune(std::string_view /*presetId*/, PreparedLayout const& /*layout*/) override { return false; }
      bool removePreset(std::string_view /*presetId*/) override { return false; }

      std::vector<std::pair<std::string, LayoutComponentStateDocument>> const& savedEntries() const noexcept
      {
        return _saved;
      }

    private:
      std::vector<std::pair<std::string, LayoutComponentStateDocument>> _saved;
    };

    constexpr auto kType = std::string_view{"split"};

    LayoutNode splitNode(std::string id = "main-paned")
    {
      auto node = LayoutNode{};
      node.id = std::move(id);
      node.type = std::string{kType};
      node.props["orientation"] = LayoutValue{std::string{"horizontal"}};
      node.children = {LayoutNode{.type = "spacer"}, LayoutNode{.type = "spacer"}};
      return node;
    }

    LayoutRuntimeState liveState(LayoutComponentStateStore& store, std::string preset = "classic")
    {
      auto state = LayoutRuntimeState{};
      state.activePresetId = std::move(preset);
      state.componentStateStore = &store;
      return state;
    }

    std::map<std::string, LayoutValue, std::less<>> positionState(double const percent)
    {
      return {{"positionPercent", LayoutValue{percent}}};
    }
  } // namespace

  TEST_CASE("StatefulComponentState - writes through to the store on a main surface",
            "[uimodel][unit][layout][component]")
  {
    auto store = RecordingStateStore{};
    auto runtimeState = liveState(store);
    auto const node = splitNode();

    auto component =
      StatefulComponentState{runtimeState, LayoutBuildStateView{runtimeState}, LayoutSurface::Main, node, kType};

    REQUIRE(component.canWrite());
    component.write(positionState(0.42));

    REQUIRE(store.savedEntries().size() == 1);
    CHECK(store.savedEntries().front().first == "classic");

    auto const& doc = store.savedEntries().front().second;
    CHECK(doc.preset == "classic");
    REQUIRE(doc.components.contains("main-paned"));

    auto const& entry = doc.components.at("main-paned");
    CHECK(entry.type == kType);
    CHECK(entry.stateVersion == kStateEntryVersion);
    CHECK(entry.baselineHash == componentBaselineHash(node));
    REQUIRE(entry.state.contains("positionPercent"));
  }

  TEST_CASE("StatefulComponentState - restores the entry matching id, type and baseline",
            "[uimodel][unit][layout][component]")
  {
    auto store = RecordingStateStore{};
    auto runtimeState = liveState(store);
    auto const node = splitNode();

    runtimeState.componentState.preset = "classic";
    runtimeState.componentState.components[node.id] = LayoutComponentStateEntry{
      .type = std::string{kType},
      .stateVersion = kStateEntryVersion,
      .baselineHash = componentBaselineHash(node),
      .state = positionState(0.25),
    };

    auto const component =
      StatefulComponentState{runtimeState, LayoutBuildStateView{runtimeState}, LayoutSurface::Main, node, kType};

    REQUIRE(component.restored());
    CHECK(component.restored()->type == kType);
    REQUIRE(component.restored()->state.contains("positionPercent"));
  }

  TEST_CASE("StatefulComponentState - suppresses writes the shell cannot attribute",
            "[uimodel][unit][layout][component]")
  {
    auto store = RecordingStateStore{};
    auto const node = splitNode();

    SECTION("a tooltip surface never persists")
    {
      auto runtimeState = liveState(store);
      auto component =
        StatefulComponentState{runtimeState, LayoutBuildStateView{runtimeState}, LayoutSurface::Tooltip, node, kType};

      CHECK_FALSE(component.canWrite());
      component.write(positionState(0.42));
      CHECK(store.savedEntries().empty());
    }

    SECTION("edit mode never persists")
    {
      auto runtimeState = liveState(store);
      runtimeState.editMode = true;

      auto component =
        StatefulComponentState{runtimeState, LayoutBuildStateView{runtimeState}, LayoutSurface::Main, node, kType};

      CHECK_FALSE(component.canWrite());
      component.write(positionState(0.42));
      CHECK(store.savedEntries().empty());
    }

    SECTION("an anonymous node never persists")
    {
      auto runtimeState = liveState(store);
      auto const anonymous = splitNode("");

      auto component =
        StatefulComponentState{runtimeState, LayoutBuildStateView{runtimeState}, LayoutSurface::Main, anonymous, kType};

      CHECK_FALSE(component.canWrite());
      component.write(positionState(0.42));
      CHECK(store.savedEntries().empty());
    }

    SECTION("an empty active preset never persists")
    {
      auto runtimeState = liveState(store, "");
      auto component =
        StatefulComponentState{runtimeState, LayoutBuildStateView{runtimeState}, LayoutSurface::Main, node, kType};

      CHECK_FALSE(component.canWrite());
      component.write(positionState(0.42));
      CHECK(store.savedEntries().empty());
    }

    SECTION("a missing store never persists")
    {
      auto runtimeState = liveState(store);
      runtimeState.componentStateStore = nullptr;

      auto component =
        StatefulComponentState{runtimeState, LayoutBuildStateView{runtimeState}, LayoutSurface::Main, node, kType};

      CHECK_FALSE(component.canWrite());
      component.write(positionState(0.42));
      CHECK(store.savedEntries().empty());
    }
  }

  TEST_CASE("StatefulComponentState - a stale generation cannot pollute a replaced document",
            "[uimodel][unit][layout][component]")
  {
    auto store = RecordingStateStore{};
    auto runtimeState = liveState(store);
    auto const node = splitNode();

    auto stale =
      StatefulComponentState{runtimeState, LayoutBuildStateView{runtimeState}, LayoutSurface::Main, node, kType};
    REQUIRE(stale.canWrite());

    // The shell swaps in a fresh state document (reset / load / save-defaults).
    ++runtimeState.componentStateGeneration;
    runtimeState.componentState = LayoutComponentStateDocument{};

    CHECK_FALSE(stale.canWrite());
    stale.write(positionState(0.42));
    CHECK(store.savedEntries().empty());

    // A component built after the swap writes normally.
    auto fresh =
      StatefulComponentState{runtimeState, LayoutBuildStateView{runtimeState}, LayoutSurface::Main, node, kType};
    REQUIRE(fresh.canWrite());
    fresh.write(positionState(0.42));
    CHECK(store.savedEntries().size() == 1);
  }

  TEST_CASE("StatefulComponentState - a candidate build reads and stamps its own preset",
            "[uimodel][unit][layout][component]")
  {
    auto store = RecordingStateStore{};
    auto runtimeState = liveState(store, "classic");
    auto const node = splitNode();

    auto candidateDoc = LayoutComponentStateDocument{};
    candidateDoc.preset = "modern";
    candidateDoc.components[node.id] = LayoutComponentStateEntry{
      .type = std::string{kType},
      .stateVersion = kStateEntryVersion,
      .baselineHash = componentBaselineHash(node),
      .state = positionState(0.75),
    };

    auto component =
      StatefulComponentState{runtimeState,
                             LayoutBuildStateView{"modern", candidateDoc, runtimeState.componentStateGeneration},
                             LayoutSurface::Main,
                             node,
                             kType};

    REQUIRE(component.restored());
    REQUIRE(component.canWrite());

    component.write(positionState(0.5));

    REQUIRE(store.savedEntries().size() == 1);
    CHECK(store.savedEntries().front().first == "modern");
    CHECK(store.savedEntries().front().second.preset == "modern");
  }

  TEST_CASE("StatefulComponentState - a candidate built against an older generation cannot write",
            "[uimodel][unit][layout][component]")
  {
    auto store = RecordingStateStore{};
    auto runtimeState = liveState(store);
    runtimeState.componentStateGeneration = 7;

    auto const node = splitNode();
    auto const candidateDoc = LayoutComponentStateDocument{};

    auto component = StatefulComponentState{
      runtimeState, LayoutBuildStateView{"classic", candidateDoc, 6}, LayoutSurface::Main, node, kType};

    CHECK_FALSE(component.canWrite());
    component.write(positionState(0.42));
    CHECK(store.savedEntries().empty());
  }
} // namespace ao::uimodel::test
