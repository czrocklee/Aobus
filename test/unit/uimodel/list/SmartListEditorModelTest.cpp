// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/list/SmartListEditorModel.h>

#include <catch2/catch_test_macros.hpp>

using namespace ao;

namespace ao::uimodel::list::test
{
  TEST_CASE("SmartListEditorModel - effective expression", "[unit][uimodel][list]")
  {
    SECTION("Local expression only")
    {
      CHECK(SmartListEditorModel::composeEffectiveExpression("", "$artist = 'Queen'") == "$artist = 'Queen'");
    }

    SECTION("Parent and local expression")
    {
      auto const effective = SmartListEditorModel::composeEffectiveExpression("$year > 1970", "$artist = 'Queen'");
      CHECK(effective == "($year > 1970) and ($artist = 'Queen')");
    }

    SECTION("Parent only")
    {
      CHECK(SmartListEditorModel::composeEffectiveExpression("$year > 1970", "") == "$year > 1970");
    }
  }

  TEST_CASE("SmartListEditorModel - preview status", "[unit][uimodel][list]")
  {
    CHECK(static_cast<int>(SmartListStatus::Valid) == 1);
  }

  TEST_CASE("SmartListEditorModel - canSubmit", "[unit][uimodel][list]")
  {
    SECTION("Valid name and status")
    {
      CHECK(SmartListEditorModel::canSubmit("My Smart List", SmartListStatus::Valid) == true);
    }

    SECTION("Empty name should be rejected")
    {
      CHECK(SmartListEditorModel::canSubmit("", SmartListStatus::Valid) == false);
    }

    SECTION("Invalid expression should be rejected")
    {
      CHECK(SmartListEditorModel::canSubmit("My List", SmartListStatus::InvalidExpression) == false);
    }

    SECTION("Empty name with invalid expression should be rejected")
    {
      CHECK(SmartListEditorModel::canSubmit("", SmartListStatus::InvalidExpression) == false);
    }

    SECTION("Empty source is treated as valid name")
    {
      CHECK(SmartListEditorModel::canSubmit("My List", SmartListStatus::EmptySource) == true);
    }
  }

  TEST_CASE("SmartListEditorModel - createDraft", "[unit][uimodel][list]")
  {
    auto const parentListId = ListId{10};
    auto const editListId = ListId{42};
    auto const name = std::string{"My Smart List"};
    auto const description = std::string{"A description"};
    auto const expression = std::string{"$artist = 'Queen'"};

    SECTION("Full draft with all fields")
    {
      auto const draft = SmartListEditorModel::createDraft(parentListId, editListId, name, description, expression);

      CHECK(draft.kind == rt::LibraryMutationService::ListKind::Smart);
      CHECK(draft.parentId == parentListId);
      CHECK(draft.listId == editListId);
      CHECK(draft.name == name);
      CHECK(draft.description == description);
      CHECK(draft.expression == expression);
    }

    SECTION("Draft for creating (listId = kInvalidListId)")
    {
      auto const draft = SmartListEditorModel::createDraft(parentListId, kInvalidListId, name, description, expression);

      CHECK(draft.kind == rt::LibraryMutationService::ListKind::Smart);
      CHECK(draft.parentId == parentListId);
      CHECK(draft.listId == kInvalidListId);
      CHECK(draft.name == name);
      CHECK(draft.description == description);
      CHECK(draft.expression == expression);
    }

    SECTION("Draft with empty strings")
    {
      auto const draft = SmartListEditorModel::createDraft(kInvalidListId, kInvalidListId, "", "", "");

      CHECK(draft.kind == rt::LibraryMutationService::ListKind::Smart);
      CHECK(draft.parentId == kInvalidListId);
      CHECK(draft.listId == kInvalidListId);
      CHECK(draft.name.empty());
      CHECK(draft.description.empty());
      CHECK(draft.expression.empty());
    }
  }
} // namespace ao::uimodel::list::test
