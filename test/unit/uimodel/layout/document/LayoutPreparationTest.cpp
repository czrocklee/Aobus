// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/Error.h>
#include <ao/uimodel/layout/document/LayoutDocument.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/layout/document/LayoutPreparation.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string>
#include <utility>

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
    auto const rejected = prepareLayout(document, limits);

    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().code == Error::Code::ValueTooLarge);
  }

  TEST_CASE("LayoutPreparation - enforces depth and value-byte limits at exact boundaries",
            "[uimodel][unit][layout][document]")
  {
    SECTION("Depth")
    {
      auto limits = generousLimits();
      limits.authored.maxDepth = 3;
      limits.effective.maxDepth = 3;

      REQUIRE(prepareLayout(documentWithDepth(3), limits));

      auto const rejected = prepareLayout(documentWithDepth(4), limits);
      REQUIRE_FALSE(rejected);
      CHECK(rejected.error().code == Error::Code::ValueTooLarge);
    }

    SECTION("Owned string bytes")
    {
      auto limits = generousLimits();
      limits.authored.maxValueBytes = 7;
      limits.effective.maxValueBytes = 7;

      auto document = LayoutDocument{};
      document.root = LayoutNode{.id = "x", .type = "spacer"};
      REQUIRE(prepareLayout(document, limits));

      document.root.id = "xx";
      auto const rejected = prepareLayout(document, limits);
      REQUIRE_FALSE(rejected);
      CHECK(rejected.error().code == Error::Code::ValueTooLarge);
    }
  }

  TEST_CASE("LayoutPreparation - charges unused authored templates", "[uimodel][unit][layout][document]")
  {
    auto limits = generousLimits();
    limits.authored.maxEntries = 2;

    auto document = LayoutDocument{};
    document.root.type = "spacer";
    document.templates["unused"] = LayoutNode{.type = "spacer"};

    auto const rejected = prepareLayout(document, limits);

    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().code == Error::Code::ValueTooLarge);
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

    auto const rejected = prepareLayout(document, limits);

    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().code == Error::Code::ValueTooLarge);
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
    auto const rejected = prepareLayout(document, rejectedLimits);

    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().code == Error::Code::ValueTooLarge);
  }
} // namespace ao::uimodel::test
