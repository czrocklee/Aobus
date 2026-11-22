// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/uimodel/library/list/ListAuthoring.h>

#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Task.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/WritableTagList.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryCommands.h>

#include <cstddef>
#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace ao::uimodel
{
  ListDeletionLabels listDeletionLabels(bool const includeDescendants) noexcept
  {
    using i18n::MessageId;
    return includeDescendants
             ? ListDeletionLabels{.title = MessageId::ListDeleteSubtreeTitle, .action = MessageId::ListDeleteAllAction}
             : ListDeletionLabels{};
  }

  std::string formatListDeletionQuestion(i18n::MessageCatalog const& catalog, rt::DeleteListReply const& root)
  {
    return i18n::requiredFormat(catalog, i18n::MessageId::ListDeleteQuestion, {{"name", root.name}});
  }

  std::string formatListSubtreeDeletionQuestion(i18n::MessageCatalog const& catalog,
                                                std::size_t const count,
                                                std::string_view const entries)
  {
    return i18n::requiredFormat(
      catalog, i18n::MessageId::ListDeleteSubtreeQuestion, {{"count", count}, {"entries", entries}});
  }

  ListDeletionCleanupText describeListDeletionCleanup(i18n::MessageCatalog const& catalog,
                                                      rt::DeleteListReply::TagImpact const& impact,
                                                      std::string_view const references)
  {
    auto const tag = rt::serializeTagExpression(impact.tag);
    auto text = ListDeletionCleanupText{};
    text.question =
      i18n::requiredFormat(catalog, i18n::MessageId::ListRemoveTag, {{"tag", tag}, {"count", impact.taggedTrackCount}});

    if (!impact.otherListReferences.empty())
    {
      text.referencesWarning =
        i18n::requiredFormat(catalog, i18n::MessageId::ListTagReferences, {{"tag", tag}, {"references", references}});
    }

    return text;
  }

  ListDeletionDescription describeListDeletion(i18n::MessageCatalog const& catalog,
                                               rt::DeleteListSubtreeReply const& preview,
                                               bool const includeDescendants)
  {
    AO_INVARIANT(!preview.deletedLists.empty() && preview.deletedLists.front().listId == preview.rootListId,
                 "A successful List deletion preview must start with its root List");
    auto const& root = preview.deletedLists.front();
    auto description = ListDeletionDescription{.labels = listDeletionLabels(includeDescendants)};

    if (includeDescendants)
    {
      auto entries = std::string{};

      for (auto const& list : preview.deletedLists)
      {
        entries.append(std::format("• {} ({})\n", list.name, list.listId.raw()));
      }

      description.question = formatListSubtreeDeletionQuestion(catalog, preview.deletedLists.size(), entries);
    }
    else
    {
      description.question = formatListDeletionQuestion(catalog, root);
    }

    if (auto const& optImpact = root.optTagImpact; optImpact)
    {
      auto references = std::string{};

      for (auto const& reference : optImpact->otherListReferences)
      {
        if (!references.empty())
        {
          references.append(", ");
        }

        references.append(reference.name);
      }

      description.optCleanup = describeListDeletionCleanup(catalog, *optImpact, references);
    }

    return description;
  }

  async::Task<Result<ListId>> saveListAsync(rt::Library* const library, rt::ListDraft draft)
  {
    auto& commands = library->commands();

    if (draft.listId == kInvalidListId)
    {
      co_return co_await commands.createListAsync(std::move(draft));
    }

    auto const listId = draft.listId;

    if (auto res = co_await commands.updateListAsync(std::move(draft)); !res)
    {
      co_return std::unexpected{std::move(res).error()};
    }

    co_return listId;
  }

  async::Task<Result<rt::DeleteListSubtreeReply>> previewListDeletionAsync(rt::Library* const library,
                                                                           ListId const listId,
                                                                           bool const includeDescendants)
  {
    auto& commands = library->commands();

    if (includeDescendants)
    {
      co_return co_await commands.previewDeleteListAndDescendantsAsync(listId);
    }

    auto res = co_await commands.previewDeleteListAsync(listId);

    if (!res)
    {
      co_return std::unexpected{std::move(res).error()};
    }

    co_return rt::DeleteListSubtreeReply{.rootListId = listId, .deletedLists = {std::move(*res)}};
  }

  async::Task<Result<rt::DeleteListSubtreeReply>> deleteListAsync(rt::Library* const library,
                                                                  ListId const listId,
                                                                  bool const includeDescendants,
                                                                  rt::DeleteListOptions const options)
  {
    auto& commands = library->commands();

    if (includeDescendants)
    {
      co_return co_await commands.deleteListAndDescendantsAsync(listId, options);
    }

    auto res = co_await commands.deleteListAsync(listId, options);

    if (!res)
    {
      co_return std::unexpected{std::move(res).error()};
    }

    co_return rt::DeleteListSubtreeReply{.rootListId = listId, .deletedLists = {std::move(*res)}};
  }
} // namespace ao::uimodel
