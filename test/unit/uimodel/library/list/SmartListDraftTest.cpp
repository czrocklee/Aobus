// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/uimodel/library/list/SmartListDraft.h>
#include <ao/uimodel/library/list/SmartListPreview.h>

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace ao;

namespace ao::uimodel::test
{
  TEST_CASE("SmartListDraft - accepts named valid draft", "[uimodel][unit][list]")
  {
    CHECK(canSubmitSmartListDraft("My Smart List", SmartListPreviewStatus::Valid) == true);
  }

  TEST_CASE("SmartListDraft - rejects empty name", "[uimodel][unit][list]")
  {
    CHECK(canSubmitSmartListDraft("", SmartListPreviewStatus::Valid) == false);
  }

  TEST_CASE("SmartListDraft - rejects invalid expression", "[uimodel][unit][list]")
  {
    CHECK(canSubmitSmartListDraft("My List", SmartListPreviewStatus::InvalidExpression) == false);
  }

  TEST_CASE("SmartListDraft - rejects empty invalid draft", "[uimodel][unit][list]")
  {
    CHECK(canSubmitSmartListDraft("", SmartListPreviewStatus::InvalidExpression) == false);
  }

  TEST_CASE("SmartListDraft - accepts named draft with unavailable preview source", "[uimodel][unit][list]")
  {
    CHECK(canSubmitSmartListDraft("My List", SmartListPreviewStatus::PreviewSourceUnavailable) == true);
  }

  TEST_CASE("SmartListDraft - preserves parent edit id and filter fields", "[uimodel][unit][list]")
  {
    auto const parentListId = ListId{10};
    auto const editListId = ListId{42};
    auto const name = std::string{"My Smart List"};
    auto const description = std::string{"A description"};
    auto const expression = std::string{"$artist = 'Queen'"};

    auto const draft = makeSmartListDraft(parentListId, editListId, name, description, expression);

    CHECK(draft.kind == rt::LibraryWriter::ListKind::Smart);
    CHECK(draft.parentId == parentListId);
    CHECK(draft.listId == editListId);
    CHECK(draft.name == name);
    CHECK(draft.description == description);
    CHECK(draft.expression == expression);
  }

  TEST_CASE("SmartListDraft - preserves create sentinel id", "[uimodel][unit][list]")
  {
    auto const parentListId = ListId{10};
    auto const name = std::string{"My Smart List"};
    auto const description = std::string{"A description"};
    auto const expression = std::string{"$artist = 'Queen'"};

    auto const draft = makeSmartListDraft(parentListId, kInvalidListId, name, description, expression);

    CHECK(draft.kind == rt::LibraryWriter::ListKind::Smart);
    CHECK(draft.parentId == parentListId);
    CHECK(draft.listId == kInvalidListId);
    CHECK(draft.name == name);
    CHECK(draft.description == description);
    CHECK(draft.expression == expression);
  }

  TEST_CASE("SmartListDraft - preserves empty text fields", "[uimodel][unit][list]")
  {
    auto const draft = makeSmartListDraft(kInvalidListId, kInvalidListId, "", "", "");

    CHECK(draft.kind == rt::LibraryWriter::ListKind::Smart);
    CHECK(draft.parentId == kInvalidListId);
    CHECK(draft.listId == kInvalidListId);
    CHECK(draft.name.empty());
    CHECK(draft.description.empty());
    CHECK(draft.expression.empty());
  }
} // namespace ao::uimodel::test
