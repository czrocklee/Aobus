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

#include <array>
#include <cstdint>
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
    class RecordingStateStore final : public LayoutComponentStateStore
    {
    public:
      std::optional<LayoutComponentStateDocument> load(std::string_view /*presetId*/) const override { return {}; }

      void save(std::string_view const presetId, LayoutComponentStateDocument const& document) override
      {
        _saved.emplace_back(std::string{presetId}, document);
      }

      bool prune(std::string_view /*presetId*/,
                 PreparedLayout const& /*layout*/,
                 LayoutSchema const& /*schema*/) override
      {
        return false;
      }

      bool removePreset(std::string_view /*presetId*/) override { return false; }

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
    auto state = LayoutComponentStateDocument{.preset = "modern"};
    auto moved = std::string{};

    auto const optInitial = session.buildSnapshot(state, false);
    REQUIRE(optInitial);
    session.apply({}, state, optInitial->generation());
    session.setEditMode(true, [&moved](std::string const& nodeId, std::int32_t, std::int32_t) { moved = nodeId; });

    auto const optSnapshot = session.buildSnapshot();
    REQUIRE(optSnapshot);
    CHECK(optSnapshot->presetId() == "modern");
    CHECK(optSnapshot->generation() == session.generation() + 1);
    CHECK(optSnapshot->isEditMode());
    REQUIRE(optSnapshot->onNodeMoved());

    state.preset = "changed-after-capture";
    CHECK(optSnapshot->componentState().preset == "modern");
    optSnapshot->onNodeMoved()("soul", 10, 20);
    CHECK(moved == "soul");
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
    CHECK(session.layout().root.children.front().id == "library-panel");
    CHECK(session.componentState().preset == "modern");

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
    REQUIRE(store.saved().size() == 1);
    CHECK(store.saved().front().first == "modern");
    auto const& saved = store.saved().front().second.components.at("library-panel");
    CHECK(saved.type == "split");
    CHECK(saved.stateVersion == kStateEntryVersion);
    CHECK(saved.baselineHash == componentBaselineHash(node));
    CHECK(saved.state.at("positionPercent").asDouble() == 0.5);
  }

  TEST_CASE("ComponentStateBinding - writes require an attributable active generation",
            "[uimodel][unit][layout][session]")
  {
    auto store = RecordingStateStore{};
    auto const node = splitNode();

    SECTION("a stale candidate cannot write after a successor commits")
    {
      auto session = LayoutSession{&store};
      auto const classicState = LayoutComponentStateDocument{.preset = "classic"};
      auto const optStaleSnapshot = session.buildSnapshot(classicState, false);
      REQUIRE(optStaleSnapshot);
      session.apply({}, classicState, optStaleSnapshot->generation());
      auto stale = session.stateFor(*optStaleSnapshot, LayoutSurface::Main, node, "split");
      REQUIRE(stale.canWrite());

      auto const modernState = LayoutComponentStateDocument{.preset = "modern"};
      auto const optSuccessor = session.buildSnapshot(modernState, false);
      REQUIRE(optSuccessor);
      session.apply({}, modernState, optSuccessor->generation());

      CHECK_FALSE(stale.canWrite());
      stale.write(positionState(0.42));
      CHECK(store.saved().empty());
    }

    SECTION("tooltip, edit-mode, anonymous, and empty-preset bindings cannot write")
    {
      auto session = LayoutSession{&store};
      auto const assertSuppressed = [&](LayoutSurface const surface,
                                        LayoutNode const& candidateNode,
                                        std::string_view const preset,
                                        bool const editMode)
      {
        auto const state = LayoutComponentStateDocument{.preset = std::string{preset}};
        auto const optSnapshot = session.buildSnapshot(state, editMode);
        REQUIRE(optSnapshot);
        session.advanceGeneration(optSnapshot->generation());
        auto binding = session.stateFor(*optSnapshot, surface, candidateNode, "split");
        CHECK_FALSE(binding.canWrite());
        binding.write(positionState(0.42));
      };

      assertSuppressed(LayoutSurface::Tooltip, node, "classic", false);
      assertSuppressed(LayoutSurface::Main, node, "classic", true);
      assertSuppressed(LayoutSurface::Main, splitNode(""), "classic", false);
      assertSuppressed(LayoutSurface::Main, node, "", false);
      CHECK(store.saved().empty());
    }

    SECTION("a missing store cannot write")
    {
      auto session = LayoutSession{};
      auto const optSnapshot = session.buildSnapshot(LayoutComponentStateDocument{.preset = "classic"}, false);
      REQUIRE(optSnapshot);
      session.advanceGeneration(optSnapshot->generation());
      auto binding = session.stateFor(*optSnapshot, LayoutSurface::Main, node, "split");
      CHECK_FALSE(binding.canWrite());
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
