// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/uimodel/layout/shell/LayoutSession.h>

#include <ao/uimodel/layout/component/LayoutComponentState.h>
#include <ao/uimodel/layout/component/LayoutComponentStateStore.h>
#include <ao/uimodel/layout/component/LayoutSchema.h>
#include <ao/uimodel/layout/component/LayoutSurface.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/layout/document/LayoutPreparation.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace ao::uimodel::test
{
  static_assert(!std::is_copy_constructible_v<LayoutSession>);
  static_assert(!std::is_copy_assignable_v<LayoutSession>);
  static_assert(!std::is_move_constructible_v<LayoutSession>);
  static_assert(!std::is_move_assignable_v<LayoutSession>);
  namespace
  {
    class RecordingStateStore final : public LayoutComponentStateStore
    {
    public:
      std::optional<LayoutComponentStateDocument> load(std::string_view /*presetId*/) const override { return {}; }

      void save(std::string_view const presetId, LayoutComponentStateDocument const& document) override
      {
        _saved.emplace_back(std::string{presetId}, document);
      }

      bool tryPrune(std::string_view /*presetId*/,
                    PreparedLayout const& /*layout*/,
                    LayoutSchema const& /*schema*/) override
      {
        return false;
      }

      bool tryRemovePreset(std::string_view /*presetId*/) override { return false; }

      std::vector<std::pair<std::string, LayoutComponentStateDocument>> const& saved() const { return _saved; }

    private:
      std::vector<std::pair<std::string, LayoutComponentStateDocument>> _saved;
    };

    LayoutNode splitNode(std::string id = "library-panel")
    {
      auto node = LayoutNode{};
      node.id = std::move(id);
      node.type = "split";
      node.props["orientation"] = LayoutValue{std::string{"horizontal"}};
      node.props["position"] = LayoutValue{std::int64_t{240}};
      node.children = {LayoutNode{.type = "spacer"}, LayoutNode{.type = "spacer"}};
      return node;
    }

    LayoutDocument panelLayout()
    {
      auto document = LayoutDocument{};
      document.root.type = "box";
      document.root.children.push_back(splitNode());
      return document;
    }

    LayoutComponentStateDocument panelState(LayoutDocument const& document, double const percent = 0.68)
    {
      auto state = LayoutComponentStateDocument{.preset = "modern"};
      REQUIRE(document.root.children.size() == 1);
      auto const& split = document.root.children.front();
      state.components[split.id] = LayoutComponentStateEntry{
        .type = "split",
        .stateVersion = kStateEntryVersion,
        .baselineHash = componentBaselineHash(split),
        .state = {{"positionPercent", LayoutValue{percent}}},
      };
      return state;
    }

    std::map<std::string, LayoutValue, std::less<>> positionState(double const percent)
    {
      return {{"positionPercent", LayoutValue{percent}}};
    }

    std::uint64_t applyCandidate(LayoutSession& session,
                                 std::string preset,
                                 LayoutDocument document,
                                 LayoutComponentStateDocument state)
    {
      state.preset = std::move(preset);
      auto const optSnapshot = session.buildSnapshot(state, false);
      REQUIRE(optSnapshot);
      auto const generation = optSnapshot->generation();
      session.apply(std::move(document), std::move(state), generation);
      return generation;
    }

    void checkStaticWriteSuppression(LayoutSurface const surface,
                                     LayoutNode const& node,
                                     std::string preset,
                                     bool const editMode)
    {
      auto store = RecordingStateStore{};
      auto session = LayoutSession{&store};
      auto state = LayoutComponentStateDocument{.preset = std::move(preset)};
      auto const optSnapshot = session.buildSnapshot(state, editMode);
      REQUIRE(optSnapshot);
      session.apply({}, state, optSnapshot->generation());
      auto binding = session.stateFor(*optSnapshot, surface, node, "split");

      CHECK_FALSE(binding.canWrite());
      binding.write(positionState(0.42));
      CHECK(session.componentState().components.empty());
      CHECK(store.saved().empty());
    }
  } // namespace

  TEST_CASE("LayoutSession - preset selection is deterministic", "[uimodel][unit][layout][session]")
  {
    static constexpr auto kSupported = std::array<std::string_view, 2>{"classic", "modern"};

    auto const modern = LayoutSession::selectPreset("modern", kSupported);
    CHECK(modern.presetId == "modern");
    CHECK_FALSE(modern.usedFallback);

    auto const empty = LayoutSession::selectPreset("", kSupported);
    CHECK(empty.presetId == "classic");
    CHECK_FALSE(empty.usedFallback);

    auto const unknown = LayoutSession::selectPreset("wide", kSupported);
    CHECK(unknown.presetId == "classic");
    CHECK(unknown.usedFallback);
    CHECK(LayoutSession::activeOrDefaultPresetId("") == "classic");
    CHECK(LayoutSession::activeOrDefaultPresetId("modern") == "modern");
  }

  TEST_CASE("LayoutSession - build snapshots own candidate state and capture edit behavior",
            "[uimodel][unit][layout][session]")
  {
    auto session = LayoutSession{};
    auto candidate = LayoutComponentStateDocument{.preset = "modern"};
    candidate.components["panel"] =
      LayoutComponentStateEntry{.type = "split", .stateVersion = kStateEntryVersion, .state = positionState(0.25)};

    auto const optCandidateSnapshot = session.buildSnapshot(candidate, false);
    REQUIRE(optCandidateSnapshot);
    session.apply({}, candidate, optCandidateSnapshot->generation());
    candidate.preset = "changed-after-capture";
    candidate.components.at("panel").state = positionState(0.75);

    CHECK(optCandidateSnapshot->componentState().preset == "modern");
    REQUIRE(optCandidateSnapshot->componentState().components.contains("panel"));
    CHECK(optCandidateSnapshot->componentState().components.at("panel").state.at("positionPercent").asDouble() == 0.25);

    auto moved = std::string{};
    bool replacementCalled = false;
    session.setEditMode(true, [&moved](std::string const& nodeId, std::int32_t, std::int32_t) { moved = nodeId; });
    auto const optSnapshot = session.buildSnapshot();
    REQUIRE(optSnapshot);
    session.setEditMode(
      true, [&replacementCalled](std::string const&, std::int32_t, std::int32_t) { replacementCalled = true; });

    CHECK(optSnapshot->presetId() == "modern");
    CHECK(optSnapshot->generation() == session.generation() + 1);
    CHECK(optSnapshot->isEditMode());
    REQUIRE(optSnapshot->onNodeMoved());
    optSnapshot->onNodeMoved()("soul", 10, 20);
    CHECK(moved == "soul");
    CHECK_FALSE(replacementCalled);
  }

  TEST_CASE("LayoutSession - applying a candidate advances the generation and replaces the session atomically",
            "[uimodel][unit][layout][session]")
  {
    auto session = LayoutSession{};
    auto document = panelLayout();
    auto state = panelState(document);
    auto const generation = applyCandidate(session, "modern", document, state);

    CHECK(session.generation() == generation);
    CHECK(session.presetId() == "modern");
    REQUIRE(session.layout().root.children.size() == 1);
    CHECK(session.layout().root.children.front().id == "library-panel");
    CHECK(session.componentState().preset == "modern");
    REQUIRE(session.componentState().components.contains("library-panel"));
    CHECK(session.componentState().components.at("library-panel").state.at("positionPercent").asDouble() == 0.68);

    auto const optNext = session.buildSnapshot();
    REQUIRE(optNext);
    CHECK(optNext->generation() == generation + 1);
  }

  TEST_CASE("LayoutSession - a candidate owns its preset when it replaces a different active preset",
            "[uimodel][unit][layout][session]")
  {
    auto store = RecordingStateStore{};
    auto session = LayoutSession{&store};
    applyCandidate(session, "classic", {}, {});

    auto const node = splitNode();
    auto const modernState = LayoutComponentStateDocument{.preset = "modern"};
    auto const optModern = session.buildSnapshot(modernState, false);
    REQUIRE(optModern);
    CHECK(optModern->presetId() == "modern");
    auto binding = session.stateFor(*optModern, LayoutSurface::Main, node, "split");

    session.apply({}, modernState, optModern->generation());
    CHECK(session.presetId() == "modern");
    REQUIRE(binding.canWrite());
    binding.write(positionState(0.42));

    REQUIRE(store.saved().size() == 1);
    CHECK(store.saved().front().first == "modern");
    CHECK(store.saved().front().second.preset == "modern");
  }

  TEST_CASE("ComponentStateBinding - matching state restores and writes through the session store",
            "[uimodel][unit][layout][session]")
  {
    auto store = RecordingStateStore{};
    auto session = LayoutSession{&store};
    auto document = panelLayout();
    auto state = panelState(document, 0.25);
    auto const node = document.root.children.front();

    auto const optSnapshot = session.buildSnapshot(state, false);
    REQUIRE(optSnapshot);
    session.apply(document, state, optSnapshot->generation());

    auto binding = session.stateFor(*optSnapshot, LayoutSurface::Main, node, "split");
    REQUIRE(binding.restored());
    CHECK(binding.restored()->state.at("positionPercent").asDouble() == 0.25);
    REQUIRE(binding.canWrite());

    binding.write(positionState(0.5));
    REQUIRE(session.componentState().components.contains("library-panel"));
    auto const& active = session.componentState().components.at("library-panel");
    CHECK(active.type == "split");
    CHECK(active.stateVersion == kStateEntryVersion);
    CHECK(active.baselineHash == componentBaselineHash(node));
    CHECK(active.state.at("positionPercent").asDouble() == 0.5);

    REQUIRE(store.saved().size() == 1);
    CHECK(store.saved().front().first == "modern");
    REQUIRE(store.saved().front().second.components.contains("library-panel"));
    auto const& saved = store.saved().front().second.components.at("library-panel");
    CHECK(saved.type == "split");
    CHECK(saved.stateVersion == kStateEntryVersion);
    CHECK(saved.baselineHash == componentBaselineHash(node));
    CHECK(saved.state.at("positionPercent").asDouble() == 0.5);
  }

  TEST_CASE("ComponentStateBinding - a successor generation fences stale writes",
            "[uimodel][unit][layout][session][async]")
  {
    auto store = RecordingStateStore{};
    auto session = LayoutSession{&store};
    auto const node = splitNode();
    auto const classicState = LayoutComponentStateDocument{.preset = "classic"};
    auto const optStaleSnapshot = session.buildSnapshot(classicState, false);
    REQUIRE(optStaleSnapshot);
    session.apply({}, classicState, optStaleSnapshot->generation());
    auto stale = session.stateFor(*optStaleSnapshot, LayoutSurface::Main, node, "split");
    REQUIRE(stale.canWrite());

    auto const* const successorPreset = GENERATE("classic", "modern");
    auto const successorState = LayoutComponentStateDocument{.preset = successorPreset};
    auto const optSuccessor = session.buildSnapshot(successorState, false);
    REQUIRE(optSuccessor);
    session.apply({}, successorState, optSuccessor->generation());

    CHECK_FALSE(stale.canWrite());
    stale.write(positionState(0.42));
    CHECK(session.componentState().components.empty());
    CHECK(store.saved().empty());
  }

  TEST_CASE("ComponentStateBinding - static eligibility prerequisites independently suppress writes",
            "[uimodel][unit][layout][session]")
  {
    auto const node = splitNode();

    SECTION("tooltip surface")
    {
      checkStaticWriteSuppression(LayoutSurface::Tooltip, node, "classic", false);
    }

    SECTION("edit mode")
    {
      checkStaticWriteSuppression(LayoutSurface::Main, node, "classic", true);
    }

    SECTION("anonymous node")
    {
      checkStaticWriteSuppression(LayoutSurface::Main, splitNode(""), "classic", false);
    }

    SECTION("empty preset")
    {
      checkStaticWriteSuppression(LayoutSurface::Main, node, "", false);
    }

    SECTION("active preset mismatch")
    {
      auto store = RecordingStateStore{};
      auto session = LayoutSession{&store};
      auto const classicState = LayoutComponentStateDocument{.preset = "classic"};
      auto const optSnapshot = session.buildSnapshot(classicState, false);
      REQUIRE(optSnapshot);
      session.apply({}, LayoutComponentStateDocument{.preset = "modern"}, optSnapshot->generation());
      auto binding = session.stateFor(*optSnapshot, LayoutSurface::Main, node, "split");

      CHECK_FALSE(binding.canWrite());
      binding.write(positionState(0.42));
      CHECK(session.componentState().components.empty());
      CHECK(store.saved().empty());
    }

    SECTION("missing store")
    {
      auto session = LayoutSession{};
      auto const state = LayoutComponentStateDocument{.preset = "classic"};
      auto const optSnapshot = session.buildSnapshot(state, false);
      REQUIRE(optSnapshot);
      session.apply({}, state, optSnapshot->generation());
      auto binding = session.stateFor(*optSnapshot, LayoutSurface::Main, node, "split");

      CHECK_FALSE(binding.canWrite());
      binding.write(positionState(0.42));
      CHECK(session.componentState().components.empty());
    }
  }

  TEST_CASE("LayoutSession - panel-size promotion moves runtime state into authored defaults",
            "[uimodel][unit][layout][session]")
  {
    auto session = LayoutSession{};
    auto document = panelLayout();
    auto state = panelState(document);
    applyCandidate(session, "modern", document, state);

    auto const optPromotion = session.preparePanelSizePromotion();
    REQUIRE(optPromotion);
    CHECK(optPromotion->componentState.preset == "modern");
    CHECK(optPromotion->componentState.components.empty());
    REQUIRE(optPromotion->layout.root.children.size() == 1);
    auto const& split = optPromotion->layout.root.children.front();
    CHECK_FALSE(split.props.contains("position"));
    CHECK(split.props.at("initialPositionPercent").asDouble() == 0.68);
  }

  TEST_CASE("LayoutSession - panel-size promotion reports no work without promotable state",
            "[uimodel][unit][layout][session]")
  {
    auto session = LayoutSession{};
    auto document = panelLayout();
    applyCandidate(session, "modern", document, LayoutComponentStateDocument{.preset = "modern"});

    CHECK_FALSE(session.preparePanelSizePromotion());
  }

  TEST_CASE("LayoutSession - panel-size promotion stamps the classic fallback when no preset is active",
            "[uimodel][unit][layout][session]")
  {
    auto session = LayoutSession{};
    auto document = panelLayout();
    auto state = panelState(document);
    state.preset.clear();
    auto const optSnapshot = session.buildSnapshot(state, false);
    REQUIRE(optSnapshot);
    session.apply(document, state, optSnapshot->generation());

    auto const optPromotion = session.preparePanelSizePromotion();
    REQUIRE(optPromotion);
    CHECK(optPromotion->componentState.preset == "classic");
  }
} // namespace ao::uimodel::test
