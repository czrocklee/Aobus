// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/uimodel/layout/document/LayoutPreparation.h>

#include <ao/Error.h>
#include <ao/uimodel/layout/document/LayoutDocument.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace ao::uimodel::test
{
  namespace
  {
    LayoutDocument documentWithDepth(std::size_t depth)
    {
      auto node = LayoutNode{.type = "spacer"};

      for (std::size_t current = 1; current < depth; ++current)
      {
        auto parent = LayoutNode{.type = "box"};
        parent.children.push_back(std::move(node));
        node = std::move(parent);
      }

      auto document = LayoutDocument{};
      document.root = std::move(node);
      return document;
    }

    LayoutNode templateReference(std::string id)
    {
      auto node = LayoutNode{.type = "template"};
      node.props["templateId"] = LayoutValue{std::move(id)};
      return node;
    }

    LayoutDocumentLimits generousLimits()
    {
      return LayoutDocumentLimits{
        .maxFileBytes = 1024,
        .authored = {.maxEntries = 100, .maxDepth = 100, .maxValueBytes = 1024},
        .effective = {.maxEntries = 100, .maxDepth = 100, .maxValueBytes = 1024},
      };
    }

    void requireEffectiveDepthBoundary(LayoutDocument const& document, std::size_t const exactDepth)
    {
      auto exactLimits = generousLimits();
      exactLimits.effective.maxDepth = exactDepth;
      REQUIRE(prepareLayout(document, exactLimits));

      auto rejectedLimits = exactLimits;
      rejectedLimits.effective.maxDepth = exactDepth - 1;
      auto const rejectedRes = prepareLayout(document, rejectedLimits);
      REQUIRE_FALSE(rejectedRes);
      CHECK(rejectedRes.error().code == Error::Code::ValueTooLarge);
    }

    void requireValueByteBoundary(LayoutDocument const& document, std::size_t const exactBytes)
    {
      auto exactLimits = generousLimits();
      exactLimits.authored.maxValueBytes = exactBytes;
      exactLimits.effective.maxValueBytes = exactBytes;
      REQUIRE(prepareLayout(document, exactLimits));

      auto rejectedLimits = exactLimits;
      rejectedLimits.authored.maxValueBytes = exactBytes - 1;
      rejectedLimits.effective.maxValueBytes = exactBytes - 1;
      auto const rejectedRes = prepareLayout(document, rejectedLimits);
      REQUIRE_FALSE(rejectedRes);
      CHECK(rejectedRes.error().code == Error::Code::ValueTooLarge);
    }
  } // namespace

  TEST_CASE("LayoutPreparation - enforces authored entry limits at the exact boundary",
            "[uimodel][unit][layout][document]")
  {
    auto limits = generousLimits();
    limits.authored.maxEntries = 3;

    auto document = LayoutDocument{};
    document.root.type = "box";
    document.root.children.push_back(LayoutNode{.type = "spacer"});
    document.root.children.push_back(LayoutNode{.type = "spacer"});

    REQUIRE(prepareLayout(document, limits));

    document.root.children.push_back(LayoutNode{.type = "spacer"});
    auto const rejectedRes = prepareLayout(document, limits);

    REQUIRE_FALSE(rejectedRes);
    CHECK(rejectedRes.error().code == Error::Code::ValueTooLarge);
  }

  TEST_CASE("LayoutPreparation - enforces concrete depth at the exact boundary", "[uimodel][unit][layout][document]")
  {
    auto limits = generousLimits();
    limits.authored.maxDepth = 3;
    limits.effective.maxDepth = 3;

    REQUIRE(prepareLayout(documentWithDepth(3), limits));

    auto const rejectedRes = prepareLayout(documentWithDepth(4), limits);
    REQUIRE_FALSE(rejectedRes);
    CHECK(rejectedRes.error().code == Error::Code::ValueTooLarge);
  }

  TEST_CASE("LayoutPreparation - enforces owned string bytes at the exact boundary",
            "[uimodel][unit][layout][document]")
  {
    auto document = LayoutDocument{};
    document.root = LayoutNode{.id = "x", .type = "spacer"};

    SECTION("the original fixed budget rejects a larger node id")
    {
      auto limits = generousLimits();
      limits.authored.maxValueBytes = 7;
      limits.effective.maxValueBytes = 7;
      REQUIRE(prepareLayout(document, limits));

      document.root.id = "xx";
      auto const rejectedRes = prepareLayout(document, limits);
      REQUIRE_FALSE(rejectedRes);
      CHECK(rejectedRes.error().code == Error::Code::ValueTooLarge);
    }

    SECTION("the same node requires its exact byte budget")
    {
      requireValueByteBoundary(document, 7);
    }
  }

  TEST_CASE("LayoutPreparation - meters use-site edges below one resolved template root",
            "[uimodel][unit][layout][document]")
  {
    auto document = LayoutDocument{};
    document.root = templateReference("concrete");
    document.templates["concrete"] = LayoutNode{.type = "box"};

    SECTION("appended child occupies depth three")
    {
      document.root.children.push_back(LayoutNode{.type = "spacer"});
      requireEffectiveDepthBoundary(document, 3);
    }

    SECTION("replacement tooltip occupies depth three")
    {
      document.root.optTooltip = BoxedLayoutNode{LayoutNode{.type = "spacer"}};
      requireEffectiveDepthBoundary(document, 3);
    }
  }

  TEST_CASE("LayoutPreparation - propagates resolved root depth through template aliases",
            "[uimodel][unit][layout][document]")
  {
    auto document = LayoutDocument{};
    document.root = templateReference("alias");
    document.templates["alias"] = templateReference("concrete");
    document.templates["concrete"] = LayoutNode{.type = "box"};

    SECTION("appended child occupies depth four")
    {
      document.root.children.push_back(LayoutNode{.type = "spacer"});
      requireEffectiveDepthBoundary(document, 4);
    }

    SECTION("replacement tooltip occupies depth four")
    {
      document.root.optTooltip = BoxedLayoutNode{LayoutNode{.type = "spacer"}};
      requireEffectiveDepthBoundary(document, 4);
    }
  }

  TEST_CASE("LayoutPreparation - meters every owned string category at exact boundaries",
            "[uimodel][unit][layout][document]")
  {
    SECTION("template key")
    {
      auto document = LayoutDocument{};
      document.root.type = "spacer";
      document.templates["key"] = LayoutNode{.type = "spacer"};
      requireValueByteBoundary(document, 15);
    }

    SECTION("property and layout keys with scalar strings")
    {
      auto document = LayoutDocument{};
      document.root = LayoutNode{.type = "x",
                                 .props = {{"p", LayoutValue{std::string{"vv"}}}},
                                 .layout = {{"l", LayoutValue{std::string{"www"}}}}};
      requireValueByteBoundary(document, 8);
    }

    SECTION("string-list entries and values")
    {
      auto document = LayoutDocument{};
      document.root = LayoutNode{.type = "x", .props = {{"items", LayoutValue{std::vector<std::string>{"a", "bc"}}}}};
      requireValueByteBoundary(document, 9);
    }
  }

  TEST_CASE("LayoutPreparation - charges copied and replacing template values", "[uimodel][unit][layout][document]")
  {
    auto document = LayoutDocument{};
    document.root = templateReference("base");
    document.root.props["p"] = LayoutValue{std::string{"use"}};
    document.templates["base"] = LayoutNode{.type = "x", .props = {{"p", LayoutValue{std::string{"base"}}}}};

    auto exactLimits = generousLimits();
    exactLimits.effective.maxValueBytes = 10;
    REQUIRE(prepareLayout(document, exactLimits));

    auto rejectedLimits = exactLimits;
    rejectedLimits.effective.maxValueBytes = 9;
    auto const rejectedRes = prepareLayout(document, rejectedLimits);
    REQUIRE_FALSE(rejectedRes);
    CHECK(rejectedRes.error().code == Error::Code::ValueTooLarge);
  }

  TEST_CASE("LayoutPreparation - charges unused authored templates", "[uimodel][unit][layout][document]")
  {
    auto limits = generousLimits();
    limits.authored.maxEntries = 2;

    auto document = LayoutDocument{};
    document.root.type = "spacer";
    document.templates["unused"] = LayoutNode{.type = "spacer"};

    auto const rejectedRes = prepareLayout(document, limits);

    REQUIRE_FALSE(rejectedRes);
    CHECK(rejectedRes.error().code == Error::Code::ValueTooLarge);
  }

  TEST_CASE("LayoutPreparation - bounds long acyclic template chains", "[uimodel][unit][layout][document]")
  {
    auto limits = generousLimits();
    limits.effective.maxDepth = 4;

    auto document = LayoutDocument{};
    document.root = templateReference("t0");
    document.templates["t0"] = templateReference("t1");
    document.templates["t1"] = templateReference("t2");
    document.templates["t2"] = templateReference("t3");
    document.templates["t3"] = LayoutNode{.type = "spacer"};

    auto const rejectedRes = prepareLayout(document, limits);

    REQUIRE_FALSE(rejectedRes);
    CHECK(rejectedRes.error().code == Error::Code::ValueTooLarge);
  }

  TEST_CASE("LayoutPreparation - charges every repeated template expansion", "[uimodel][unit][layout][document]")
  {
    auto document = LayoutDocument{};
    document.root.type = "box";
    document.root.children.push_back(templateReference("pair"));
    document.root.children.push_back(templateReference("pair"));
    document.templates["pair"] = LayoutNode{.type = "box"};
    document.templates["pair"].children.push_back(LayoutNode{.type = "spacer"});

    auto exactLimits = generousLimits();
    exactLimits.effective.maxEntries = 5;
    REQUIRE(prepareLayout(document, exactLimits));

    auto rejectedLimits = exactLimits;
    rejectedLimits.effective.maxEntries = 4;
    auto const rejectedRes = prepareLayout(document, rejectedLimits);

    REQUIRE_FALSE(rejectedRes);
    CHECK(rejectedRes.error().code == Error::Code::ValueTooLarge);
  }
} // namespace ao::uimodel::test
