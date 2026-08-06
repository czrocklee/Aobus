// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ListCommand.h"

#include "CliRuntime.h"
#include "CommandError.h"
#include "DryRunFlag.h"
#include "DumpOutput.h"
#include "Output.h"
#include "QueryHelp.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/ListStore.h>
#include <ao/library/ListView.h>
#include <ao/library/MusicLibrary.h>
#include <ao/query/Expression.h>
#include <ao/query/Serializer.h>
#include <ao/rt/CoreRuntime.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/ListNode.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/TrackRow.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryReader.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/rt/source/TrackSource.h>
#include <ao/rt/source/TrackSourceCache.h>
#include <ao/yaml/Reflect.h>

#include <CLI/App.hpp>
#include <gsl-lite/gsl-lite.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <ostream>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ao::cli
{
  struct ListRecordDto final
  {
    ListId id{};
    std::string name{};
    std::string description{};
    ListId parentId{};
    std::string filter{};
    std::vector<TrackId> order{};
  };

  struct ListCollectionDto final
  {
    std::vector<ListRecordDto> lists{};
  };

  struct ListTrackRowDto final
  {
    TrackId id{};
    std::string title{};
    std::string artist{};
    std::string album{};
  };

  struct ListDetailDto final
  {
    ListId id{};
    std::string name{};
    std::string description{};
    ListId parentId{};
    std::string filter{};
    std::vector<TrackId> order{};
    std::vector<ListTrackRowDto> tracks{};
  };

  struct ListDetailDocumentDto final
  {
    ListDetailDto list{};
  };

  struct ListCreateReportDto final
  {
    std::string action{};
    bool dryRun = false;
    std::optional<ListId> optListId{};
    std::string name{};
    ListId parentId{};
    std::string filter{};
  };

  struct ListUpdateReportDto final
  {
    std::string action{};
    bool dryRun = false;
    ListId listId{};
    bool changed = false;
    std::vector<rt::ListFieldChange> fields{};
  };

  struct ListDeleteReportDto final
  {
    std::string action{};
    bool dryRun = false;
    ListId listId{};
    std::string name{};
    std::size_t forgottenPositionCount = 0;
  };

  struct ListDeleteSubtreeReportDto final
  {
    std::string action{};
    bool dryRun = false;
    ListId rootListId{};
    std::vector<rt::DeleteListReply> deletedLists{};
  };

  struct ListOrderReportDto final
  {
    std::string action{};
    ListId listId{};
    std::string status{};
    std::vector<TrackId> selectedTrackIds{};
    std::optional<TrackId> optBeforeTrackId{};
    std::optional<std::size_t> optForgottenPositionCount{};
  };

  struct ListMembershipReportDto final
  {
    std::string action{};
    bool dryRun = false;
    ListId listId{};
    std::string listName{};
    std::string tag{};
    bool changed = false;
    std::vector<TrackId> targetTrackIds{};
    std::vector<rt::TrackTagsChange> changes{};
    std::vector<TrackId> forgottenPositionTrackIds{};
  };
} // namespace ao::cli

template<>
struct ao::yaml::ReflectNameOverrides<ao::cli::ListCreateReportDto>
{
  static constexpr std::string_view keyFor(std::string_view memberName) noexcept
  {
    return memberName == "optListId" ? "listId" : memberName;
  }
};

template<>
struct ao::yaml::ReflectNameOverrides<ao::cli::ListOrderReportDto>
{
  static constexpr std::string_view keyFor(std::string_view memberName) noexcept
  {
    if (memberName == "optBeforeTrackId")
    {
      return "beforeTrackId";
    }

    if (memberName == "optForgottenPositionCount")
    {
      return "forgottenPositionCount";
    }

    return memberName;
  }
};

namespace ao::cli
{
  Result<> validateListOrderCommandStatus(rt::ListOrderAuthoringStatus const status)
  {
    switch (status)
    {
      case rt::ListOrderAuthoringStatus::Applied:
      case rt::ListOrderAuthoringStatus::NoOp: return {};
      case rt::ListOrderAuthoringStatus::Stale:
        return makeError(Error::Code::Conflict, "List order target became stale");
      case rt::ListOrderAuthoringStatus::Unavailable: return makeError(Error::Code::InvalidState, "Library is busy");
    }

    gsl_Assert(false && "Unknown List order authoring status");
    std::unreachable();
  }

  namespace
  {
    std::vector<TrackId> storedOrder(library::ListView const& view)
    {
      auto const ids = view.orderTrackIds();
      return {ids.begin(), ids.end()};
    }

    void printParent(std::ostream& os, ListId const parentId)
    {
      if (parentId == kInvalidListId)
      {
        std::println(os, "all-tracks");
      }
      else
      {
        std::println(os, "{}", parentId);
      }
    }

    void printListsPlain(library::MusicLibrary const& library, std::ostream& os)
    {
      auto const transaction = library.readTransaction();
      auto const reader = library.lists().reader(transaction);
      constexpr int kIdWidth = 5;

      for (auto const& [id, view] : reader)
      {
        std::println(os, "{:>5} {}", id, view.name());
        std::print(os, "{} parent: ", std::string(kIdWidth, ' '));
        printParent(os, view.parentId());
        std::println(os, "{} filter: \"{}\"", std::string(kIdWidth, ' '), view.filter());
        std::println(os, "{} saved positions: {}", std::string(kIdWidth, ' '), view.orderTrackIds().size());

        if (!view.description().empty())
        {
          std::println(os, "{} desc: \"{}\"", std::string(kIdWidth, ' '), view.description());
        }
      }
    }

    ListRecordDto toListRecordDto(ListId const id, library::ListView const& view)
    {
      return ListRecordDto{
        .id = id,
        .name = std::string{view.name()},
        .description = std::string{view.description()},
        .parentId = view.parentId(),
        .filter = std::string{view.filter()},
        .order = storedOrder(view),
      };
    }

    void emitListCollectionDocument(library::MusicLibrary const& library, OutputFormat format, std::ostream& os)
    {
      auto const transaction = library.readTransaction();
      auto const reader = library.lists().reader(transaction);
      auto dto = ListCollectionDto{};

      for (auto const& [id, view] : reader)
      {
        dto.lists.push_back(toListRecordDto(id, view));
      }

      emitDocument(os, format, dto);
    }

    std::vector<TrackId> effectiveListTrackIds(CliRuntime& cli, ListId const listId)
    {
      auto& sources = cli.core().sources();
      sources.reloadAllTracks();
      auto sourceRes = sources.acquire(listId);

      if (!sourceRes)
      {
        throwCommandError(sourceRes.error());
      }

      auto sourceLease = std::move(*sourceRes);
      auto& source = sourceLease.source();
      auto ids = std::vector<TrackId>{};
      ids.reserve(source.size());

      for (std::size_t index = 0; index < source.size(); ++index)
      {
        ids.push_back(source.trackIdAt(index));
      }

      return ids;
    }

    std::vector<rt::TrackRow> resolveRows(rt::LibraryReader const& reader, std::span<TrackId const> ids)
    {
      auto rows = std::vector<rt::TrackRow>{};
      rows.reserve(ids.size());

      for (auto const trackId : ids)
      {
        if (auto optRow = reader.trackRow(trackId); optRow)
        {
          rows.push_back(std::move(*optRow));
        }
      }

      return rows;
    }

    ListTrackRowDto toListTrackRowDto(rt::TrackRow const& row)
    {
      return ListTrackRowDto{.id = row.id, .title = row.title, .artist = row.artist, .album = row.album};
    }

    ListDetailDto toListDetailDto(rt::ListNode const& node,
                                  std::span<TrackId const> order,
                                  std::span<rt::TrackRow const> rows)
    {
      auto dto = ListDetailDto{
        .id = node.id,
        .name = node.name,
        .description = node.description,
        .parentId = node.parentId,
        .filter = node.expression,
        .order = {order.begin(), order.end()},
      };
      dto.tracks.reserve(rows.size());

      for (auto const& row : rows)
      {
        dto.tracks.push_back(toListTrackRowDto(row));
      }

      return dto;
    }

    void printPlainTrackRows(std::ostream& os, std::span<rt::TrackRow const> rows)
    {
      for (auto const& row : rows)
      {
        std::println(os, "    {:>5} {}", row.id, row.title);
      }
    }

    void printListDetail(CliRuntime& cli, ListId const listId)
    {
      auto reader = cli.library().reader();
      auto optNode = reader.listNode(listId);

      if (!optNode)
      {
        throwCommandError(Error::Code::NotFound, "list not found: {}", listId);
      }

      auto const ids = effectiveListTrackIds(cli, listId);
      auto const rows = resolveRows(reader, ids);
      auto const order = reader.listOrderTrackIds(listId);
      auto const dto = toListDetailDto(*optNode, order, rows);

      if (cli.options().format == OutputFormat::Yaml)
      {
        emitDocument(cli.io().out, cli.options().format, ListDetailDocumentDto{.list = dto});
        return;
      }

      if (cli.options().format == OutputFormat::Json)
      {
        emitDocument(cli.io().out, cli.options().format, dto);
        return;
      }

      std::println(cli.io().out, "List ID: {}", optNode->id);
      std::println(cli.io().out, "  Name: {}", optNode->name);
      std::println(cli.io().out, "  Description: {}", optNode->description);
      std::print(cli.io().out, "  Parent: ");
      printParent(cli.io().out, optNode->parentId);
      std::println(cli.io().out, "  Filter: {}", optNode->expression);
      std::println(cli.io().out, "  Saved positions: {}", order.size());
      std::println(cli.io().out, "  Tracks: {}", rows.size());
      printPlainTrackRows(cli.io().out, rows);
    }

    rt::LibraryWriter::ListDraft draftFromNode(rt::ListNode const& node)
    {
      return rt::LibraryWriter::ListDraft{
        .parentId = node.parentId,
        .listId = node.id,
        .name = node.name,
        .description = node.description,
        .expression = node.expression,
      };
    }

    rt::LibraryWriter::ListDraft requireListDraft(ListId const listId, rt::LibraryReader& reader)
    {
      auto optNode = reader.listNode(listId);

      if (!optNode)
      {
        throwCommandError(Error::Code::NotFound, "list not found: {}", listId);
      }

      return draftFromNode(*optNode);
    }

    void printListCreateMutation(CliRuntime& cli,
                                 std::optional<ListId> optListId,
                                 rt::LibraryWriter::ListDraft const& draft,
                                 bool const dryRun)
    {
      if (cli.options().format != OutputFormat::Plain)
      {
        emitDocument(cli.io().out,
                     cli.options().format,
                     ListCreateReportDto{
                       .action = "create",
                       .dryRun = dryRun,
                       .optListId = optListId,
                       .name = draft.name,
                       .parentId = draft.parentId,
                       .filter = draft.expression,
                     });
        return;
      }

      if (optListId)
      {
        std::println(cli.io().out, "add list: {} {}{}", *optListId, draft.name, dryRun ? " (dry-run)" : "");
      }
      else
      {
        std::println(cli.io().out, "add list: {}{}", draft.name, dryRun ? " (dry-run)" : "");
      }
    }

    void createList(CliRuntime& cli,
                    std::string const& name,
                    std::string const& filter,
                    std::string const& description,
                    ListId const parentListId,
                    bool const dryRun)
    {
      auto const draft = rt::LibraryWriter::ListDraft{
        .parentId = parentListId,
        .name = name,
        .description = description,
        .expression = filter,
      };

      if (dryRun)
      {
        auto const result = cli.library().writer().previewCreateList(draft);

        if (!result)
        {
          throwCommandError(result.error());
        }

        printListCreateMutation(cli, std::nullopt, draft, true);
        return;
      }

      auto const result = cli.library().writer().createList(draft);

      if (!result)
      {
        throwCommandError(result.error());
      }

      printListCreateMutation(cli, *result, draft, false);
    }

    void printListUpdateMutation(CliRuntime& cli,
                                 ListId const listId,
                                 rt::UpdateListReply const& reply,
                                 bool const dryRun)
    {
      if (cli.options().format != OutputFormat::Plain)
      {
        emitDocument(cli.io().out,
                     cli.options().format,
                     ListUpdateReportDto{
                       .action = "update",
                       .dryRun = dryRun,
                       .listId = listId,
                       .changed = reply.changed,
                       .fields = reply.fieldChanges,
                     });
        return;
      }

      std::println(cli.io().out, "updated list: {}{}", listId, dryRun ? " (dry-run)" : "");
    }

    void updateList(CliRuntime& cli,
                    ListId const listId,
                    std::optional<std::string> const& optName,
                    std::optional<std::string> const& optDescription,
                    std::optional<std::string> const& optFilter,
                    std::optional<std::uint32_t> const& optParent,
                    bool const dryRun)
    {
      if (!optName && !optDescription && !optFilter && !optParent)
      {
        throwCommandError(Error::Code::InvalidInput, "list update requires at least one field option");
      }

      auto reader = cli.library().reader();
      auto draft = requireListDraft(listId, reader);

      if (optName)
      {
        draft.name = *optName;
      }

      if (optDescription)
      {
        draft.description = *optDescription;
      }

      if (optFilter)
      {
        draft.expression = *optFilter;
      }

      if (optParent)
      {
        draft.parentId = ListId{*optParent};
      }

      auto const result =
        dryRun ? cli.library().writer().previewUpdateList(draft) : cli.library().writer().updateList(draft);

      if (!result)
      {
        throwCommandError(result.error());
      }

      printListUpdateMutation(cli, listId, *result, dryRun);
    }

    void printListDeleteMutation(CliRuntime& cli, rt::DeleteListReply const& reply, bool const dryRun)
    {
      if (cli.options().format != OutputFormat::Plain)
      {
        emitDocument(cli.io().out,
                     cli.options().format,
                     ListDeleteReportDto{
                       .action = "delete",
                       .dryRun = dryRun,
                       .listId = reply.listId,
                       .name = reply.name,
                       .forgottenPositionCount = reply.orderTrackIdCount,
                     });
        return;
      }

      std::println(cli.io().out,
                   "deleted list: {} (forgot {} saved position{}){}",
                   reply.listId,
                   reply.orderTrackIdCount,
                   reply.orderTrackIdCount == 1 ? "" : "s",
                   dryRun ? " (dry-run)" : "");
    }

    void printListDeleteSubtreeMutation(CliRuntime& cli, rt::DeleteListSubtreeReply const& reply, bool const dryRun)
    {
      if (cli.options().format != OutputFormat::Plain)
      {
        emitDocument(cli.io().out,
                     cli.options().format,
                     ListDeleteSubtreeReportDto{
                       .action = "delete-subtree",
                       .dryRun = dryRun,
                       .rootListId = reply.rootListId,
                       .deletedLists = reply.deletedLists,
                     });
        return;
      }

      std::println(cli.io().out,
                   "{} list subtree: {} ({} List{})",
                   dryRun ? "would delete" : "deleted",
                   reply.rootListId,
                   reply.deletedLists.size(),
                   reply.deletedLists.size() == 1 ? "" : "s");

      for (auto const& deleted : reply.deletedLists)
      {
        std::println(cli.io().out,
                     "  {} {} (forget {} saved position{})",
                     deleted.listId,
                     deleted.name,
                     deleted.orderTrackIdCount,
                     deleted.orderTrackIdCount == 1 ? "" : "s");
      }
    }

    std::string_view orderStatusName(rt::ListOrderAuthoringStatus const status)
    {
      switch (status)
      {
        case rt::ListOrderAuthoringStatus::Applied: return "applied";
        case rt::ListOrderAuthoringStatus::NoOp: return "no-op";
        case rt::ListOrderAuthoringStatus::Stale: return "stale";
        case rt::ListOrderAuthoringStatus::Unavailable: return "unavailable";
      }

      return "unknown";
    }

    void requireSuccessfulListOrderStatus(rt::ListOrderAuthoringStatus const status)
    {
      if (auto result = validateListOrderCommandStatus(status); !result)
      {
        throwCommandError(result.error());
      }
    }

    std::vector<TrackId> trackIds(std::span<std::uint32_t const> rawTrackIds)
    {
      auto result = std::vector<TrackId>{};
      result.reserve(rawTrackIds.size());
      auto seen = std::unordered_set<TrackId>{};

      for (auto const rawTrackId : rawTrackIds)
      {
        if (auto const trackId = TrackId{rawTrackId}; seen.insert(trackId).second)
        {
          result.push_back(trackId);
        }
      }

      return result;
    }

    std::string tagExpression(std::string_view const tag)
    {
      return query::serialize(query::VariableExpression{.type = query::VariableType::Tag, .name = std::string{tag}});
    }

    void printListMembershipMutation(CliRuntime& cli, ListMembershipReportDto const& report)
    {
      if (cli.options().format != OutputFormat::Plain)
      {
        emitDocument(cli.io().out, cli.options().format, report);
        return;
      }

      auto const trackCount = report.targetTrackIds.size();
      auto const tag = tagExpression(report.tag);

      if (report.action == "add")
      {
        std::println(cli.io().out,
                     "Added {} to {} track{} for {}.{}",
                     tag,
                     trackCount,
                     trackCount == 1 ? "" : "s",
                     report.listName,
                     report.dryRun ? " (dry-run)" : "");
        return;
      }

      auto const forgottenCount = report.forgottenPositionTrackIds.size();

      if (forgottenCount == 0)
      {
        std::println(cli.io().out,
                     "Removed {} from {} track{} and confirmed no saved position remains in {}.{}",
                     tag,
                     trackCount,
                     trackCount == 1 ? "" : "s",
                     report.listName,
                     report.dryRun ? " (dry-run)" : "");
      }
      else
      {
        std::println(cli.io().out,
                     "Removed {} from {} track{} and forgot {} saved position{} in {}.{}",
                     tag,
                     trackCount,
                     trackCount == 1 ? "" : "s",
                     forgottenCount,
                     forgottenCount == 1 ? "" : "s",
                     report.listName,
                     report.dryRun ? " (dry-run)" : "");
      }
    }

    void updateListMembership(CliRuntime& cli,
                              ListId const listId,
                              std::span<std::uint32_t const> rawTrackIds,
                              bool const add,
                              bool const dryRun)
    {
      auto const targets = trackIds(rawTrackIds);

      if (dryRun)
      {
        if (add)
        {
          auto result = cli.library().writer().previewAddTracksToList(listId, targets);

          if (!result)
          {
            throwCommandError(result.error());
          }

          printListMembershipMutation(cli,
                                      ListMembershipReportDto{
                                        .action = "add",
                                        .dryRun = true,
                                        .listId = result->listId,
                                        .listName = result->listName,
                                        .tag = result->tag,
                                        .changed = !result->tagEdit.changes.empty(),
                                        .targetTrackIds = result->targetTrackIds,
                                        .changes = result->tagEdit.changes,
                                      });
          return;
        }

        auto result = cli.library().writer().previewRemoveTracksFromList(listId, targets);

        if (!result)
        {
          throwCommandError(result.error());
        }

        printListMembershipMutation(
          cli,
          ListMembershipReportDto{
            .action = "remove",
            .dryRun = true,
            .listId = result->listId,
            .listName = result->listName,
            .tag = result->tag,
            .changed = !result->tagEdit.changes.empty() || !result->forgottenPositionTrackIds.empty(),
            .targetTrackIds = result->targetTrackIds,
            .changes = result->tagEdit.changes,
            .forgottenPositionTrackIds = result->forgottenPositionTrackIds,
          });
        return;
      }

      auto bindingRes = cli.library().bindTrackTargets(targets);

      if (!bindingRes)
      {
        throwCommandError(bindingRes.error());
      }

      if (add)
      {
        auto result = cli.library().writer().addTracksToList(listId, *bindingRes);

        if (!result)
        {
          throwCommandError(result.error());
        }

        if (result->status == rt::TrackAuthoringStatus::Stale)
        {
          throwCommandError(Error::Code::Conflict, "List membership targets became stale");
        }

        if (result->status == rt::TrackAuthoringStatus::Unavailable)
        {
          throwCommandError(Error::Code::InvalidState, "Library is busy");
        }

        printListMembershipMutation(cli,
                                    ListMembershipReportDto{
                                      .action = "add",
                                      .listId = result->reply.listId,
                                      .listName = result->reply.listName,
                                      .tag = result->reply.tag,
                                      .changed = result->status == rt::TrackAuthoringStatus::Applied,
                                      .targetTrackIds = result->reply.targetTrackIds,
                                      .changes = result->reply.tagEdit.changes,
                                    });
        return;
      }

      auto result = cli.library().writer().removeTracksFromList(listId, *bindingRes);

      if (!result)
      {
        throwCommandError(result.error());
      }

      if (result->status == rt::TrackAuthoringStatus::Stale)
      {
        throwCommandError(Error::Code::Conflict, "List membership targets became stale");
      }

      if (result->status == rt::TrackAuthoringStatus::Unavailable)
      {
        throwCommandError(Error::Code::InvalidState, "Library is busy");
      }

      printListMembershipMutation(cli,
                                  ListMembershipReportDto{
                                    .action = "remove",
                                    .listId = result->reply.listId,
                                    .listName = result->reply.listName,
                                    .tag = result->reply.tag,
                                    .changed = result->status == rt::TrackAuthoringStatus::Applied,
                                    .targetTrackIds = result->reply.targetTrackIds,
                                    .changes = result->reply.tagEdit.changes,
                                    .forgottenPositionTrackIds = result->reply.forgottenPositionTrackIds,
                                  });
    }

    rt::BoundListOrder bindCurrentListOrder(CliRuntime& cli, ListId const listId)
    {
      auto effectiveIds = effectiveListTrackIds(cli, listId);
      auto result = cli.library().bindListOrder(listId, std::move(effectiveIds));

      if (!result)
      {
        throwCommandError(result.error());
      }

      return std::move(*result);
    }

    void printListOrderMutation(CliRuntime& cli, ListOrderReportDto const& report)
    {
      if (cli.options().format != OutputFormat::Plain)
      {
        emitDocument(cli.io().out, cli.options().format, report);
        return;
      }

      if (report.action == "move")
      {
        std::println(cli.io().out, "moved tracks in list: {} ({})", report.listId, report.status);
      }
      else
      {
        std::println(cli.io().out,
                     "{} list order: {} ({}; forgot {} position{})",
                     report.action,
                     report.listId,
                     report.status,
                     report.optForgottenPositionCount.value_or(0),
                     report.optForgottenPositionCount.value_or(0) == 1 ? "" : "s");
      }
    }

    void moveListOrder(CliRuntime& cli,
                       ListId const listId,
                       std::span<std::uint32_t const> rawTrackIds,
                       std::optional<std::uint32_t> const optRawBeforeTrackId)
    {
      auto const selectedTrackIds = trackIds(rawTrackIds);
      auto const optBeforeTrackId = optRawBeforeTrackId ? std::optional{TrackId{*optRawBeforeTrackId}} : std::nullopt;
      auto const binding = bindCurrentListOrder(cli, listId);
      auto result = cli.library().writer().moveListOrder(binding, selectedTrackIds, optBeforeTrackId);

      if (!result)
      {
        throwCommandError(result.error());
      }

      requireSuccessfulListOrderStatus(result->status);
      printListOrderMutation(cli,
                             ListOrderReportDto{
                               .action = "move",
                               .listId = listId,
                               .status = std::string{orderStatusName(result->status)},
                               .selectedTrackIds = result->reply.selectedTrackIds,
                               .optBeforeTrackId = result->reply.optBeforeTrackId,
                             });
    }

    void resetListOrder(CliRuntime& cli, ListId const listId)
    {
      auto const binding = bindCurrentListOrder(cli, listId);
      auto result = cli.library().writer().resetListOrder(binding);

      if (!result)
      {
        throwCommandError(result.error());
      }

      requireSuccessfulListOrderStatus(result->status);
      printListOrderMutation(cli,
                             ListOrderReportDto{
                               .action = "reset",
                               .listId = listId,
                               .status = std::string{orderStatusName(result->status)},
                               .optForgottenPositionCount = result->reply.forgottenPositionCount,
                             });
    }

    void forgetHiddenListOrder(CliRuntime& cli, ListId const listId)
    {
      auto const binding = bindCurrentListOrder(cli, listId);
      auto result = cli.library().writer().forgetHiddenListOrder(binding);

      if (!result)
      {
        throwCommandError(result.error());
      }

      requireSuccessfulListOrderStatus(result->status);
      printListOrderMutation(cli,
                             ListOrderReportDto{
                               .action = "forget-hidden",
                               .listId = listId,
                               .status = std::string{orderStatusName(result->status)},
                               .optForgottenPositionCount = result->reply.forgottenPositionCount,
                             });
    }

    void dumpLists(library::MusicLibrary const& library, bool const raw, OutputFormat const format, std::ostream& os)
    {
      if (raw && format != OutputFormat::Plain)
      {
        throwCommandError(Error::Code::InvalidInput, "list dump --raw supports only plain output");
      }

      auto const transaction = library.readTransaction();
      auto const reader = library.lists().reader(transaction);
      auto dto = ListCollectionDto{};

      for (auto const& [id, view] : reader)
      {
        if (format != OutputFormat::Plain && !raw)
        {
          dto.lists.push_back(toListRecordDto(id, view));
        }
        else if (raw)
        {
          std::println(os, "List ID: {}", id);
          hexDump(view.rawData(), os);
        }
        else
        {
          std::println(os, "List ID: {}", id);
          std::println(os, "  Name: {}", view.name());
          std::println(os, "  Description: {}", view.description());
          std::print(os, "  Parent: ");
          printParent(os, view.parentId());
          std::println(os, "  Filter: {}", view.filter());
          std::println(os, "  Saved positions: {}", view.orderTrackIds().size());
        }
      }

      if (format != OutputFormat::Plain && !raw)
      {
        emitDocument(os, format, dto);
      }
    }
  } // namespace

  void configureListCommand(CLI::App& app, CliRuntime& cli)
  {
    auto* list = app.add_subcommand("list", "List management commands");
    list->require_subcommand(1);

    auto* show = list->add_subcommand("show", "Show lists");
    auto* showId = show->add_option("id", "list id");
    show->callback(
      [&cli, showId]
      {
        if (showId->count() > 0)
        {
          printListDetail(cli, ListId{showId->as<std::uint32_t>()});
        }
        else if (cli.options().format == OutputFormat::Plain)
        {
          printListsPlain(cli.musicLibrary(), cli.io().out);
        }
        else
        {
          emitListCollectionDocument(cli.musicLibrary(), cli.options().format, cli.io().out);
        }
      });

    auto* create = list->add_subcommand("create", "Create a new list");
    create->footer(listCreateHelpFooter());
    auto* name = create->add_option("-n,--name", "list name")->required();
    auto* filter = create->add_option("-f,--filter", "local track filter expression");
    auto* description = create->add_option("-d,--desc", "list description");
    auto* parent = create->add_option("-p,--parent", "parent list id (0 = all-tracks)")->default_val(0);
    auto* createDryRun = addDryRunFlag(*create);
    create->callback(
      [&cli, name, filter, description, parent, createDryRun]
      {
        createList(cli,
                   name->as<std::string>(),
                   filter->as<std::string>(),
                   description->as<std::string>(),
                   ListId{parent->as<std::uint32_t>()},
                   isDryRun(createDryRun));
      });

    auto* update = list->add_subcommand("update", "Update a list");
    auto* updateId = update->add_option("id", "list id")->required();
    auto* updateName = update->add_option("--name", "list name");
    auto* updateDescription = update->add_option("--desc", "list description");
    auto* updateFilter = update->add_option("--filter", "local track filter expression; empty inherits the parent");
    auto* updateParent = update->add_option("--parent", "parent list id (0 = all-tracks)");
    auto* updateDryRun = addDryRunFlag(*update);
    update->callback(
      [&cli, updateId, updateName, updateDescription, updateFilter, updateParent, updateDryRun]
      {
        auto const optName = updateName->count() > 0 ? std::optional{updateName->as<std::string>()} : std::nullopt;
        auto const optDescription =
          updateDescription->count() > 0 ? std::optional{updateDescription->as<std::string>()} : std::nullopt;
        auto const optFilter =
          updateFilter->count() > 0 ? std::optional{updateFilter->as<std::string>()} : std::nullopt;
        auto const optParent =
          updateParent->count() > 0 ? std::optional{updateParent->as<std::uint32_t>()} : std::nullopt;
        updateList(cli,
                   ListId{updateId->as<std::uint32_t>()},
                   optName,
                   optDescription,
                   optFilter,
                   optParent,
                   isDryRun(updateDryRun));
      });

    auto* add = list->add_subcommand("add", "Add tracks to a writable-tag List");
    auto* addListId = add->add_option("listId", "list id")->required();
    auto addTrackIdsPtr = std::make_shared<std::vector<std::uint32_t>>();
    add->add_option("trackId", *addTrackIdsPtr, "track id")->required();
    auto* addDryRun = addDryRunFlag(*add);
    add->callback(
      [&cli, addListId, addTrackIdsPtr, addDryRun]
      {
        updateListMembership(cli, ListId{addListId->as<std::uint32_t>()}, *addTrackIdsPtr, true, isDryRun(addDryRun));
      });

    auto* remove =
      list->add_subcommand("remove", "Remove tracks from a writable-tag List and forget their saved positions");
    auto* removeListId = remove->add_option("listId", "list id")->required();
    auto removeTrackIdsPtr = std::make_shared<std::vector<std::uint32_t>>();
    remove->add_option("trackId", *removeTrackIdsPtr, "track id")->required();
    auto* removeDryRun = addDryRunFlag(*remove);
    remove->callback(
      [&cli, removeListId, removeTrackIdsPtr, removeDryRun]
      {
        updateListMembership(
          cli, ListId{removeListId->as<std::uint32_t>()}, *removeTrackIdsPtr, false, isDryRun(removeDryRun));
      });

    auto* order = list->add_subcommand("order", "Manage a list's saved manual order");
    order->require_subcommand(1);

    auto* move = order->add_subcommand("move", "Move tracks before an anchor, or to the bottom when omitted");
    auto* moveListId = move->add_option("listId", "list id")->required();
    auto moveTrackIdsPtr = std::make_shared<std::vector<std::uint32_t>>();
    move->add_option("trackId", *moveTrackIdsPtr, "track id")->required();
    auto* beforeTrackId = move->add_option("--before", "anchor track id");
    move->callback(
      [&cli, moveListId, moveTrackIdsPtr, beforeTrackId]
      {
        auto const optBeforeTrackId =
          beforeTrackId->count() > 0 ? std::optional{beforeTrackId->as<std::uint32_t>()} : std::nullopt;
        moveListOrder(cli, ListId{moveListId->as<std::uint32_t>()}, *moveTrackIdsPtr, optBeforeTrackId);
      });

    auto* reset = order->add_subcommand("reset", "Forget every saved position");
    auto* resetListId = reset->add_option("listId", "list id")->required();
    reset->callback([&cli, resetListId] { resetListOrder(cli, ListId{resetListId->as<std::uint32_t>()}); });

    auto* forgetHidden =
      order->add_subcommand("forget-hidden", "Forget positions for tracks outside current membership");
    auto* forgetHiddenListId = forgetHidden->add_option("listId", "list id")->required();
    forgetHidden->callback([&cli, forgetHiddenListId]
                           { forgetHiddenListOrder(cli, ListId{forgetHiddenListId->as<std::uint32_t>()}); });

    auto* del = list->add_subcommand("delete", "Delete a list");
    auto* deleteId = del->add_option("id", "list id")->required();
    auto* deleteDescendants =
      del->add_flag("--descendants", "delete the complete subtree; use --dry-run to preview it");
    auto* deleteDryRun = addDryRunFlag(*del);
    del->callback(
      [&cli, deleteId, deleteDescendants, deleteDryRun]
      {
        auto const listId = ListId{deleteId->as<std::uint32_t>()};
        auto const dryRun = isDryRun(deleteDryRun);

        if (deleteDescendants->count() > 0)
        {
          auto const result = dryRun ? cli.library().writer().previewDeleteListAndDescendants(listId)
                                     : cli.library().writer().deleteListAndDescendants(listId);

          if (!result)
          {
            throwCommandError(result.error());
          }

          printListDeleteSubtreeMutation(cli, *result, dryRun);
          return;
        }

        auto const result =
          dryRun ? cli.library().writer().previewDeleteList(listId) : cli.library().writer().deleteList(listId);

        if (!result)
        {
          throwCommandError(result.error());
        }

        printListDeleteMutation(cli, *result, dryRun);
      });

    auto* dump = list->add_subcommand("dump", "Dump lists from database");
    auto* dumpRaw = dump->add_flag("--raw", "hex dump raw bytes");
    dump->callback([&cli, dumpRaw]
                   { dumpLists(cli.musicLibrary(), dumpRaw->count() > 0, cli.options().format, cli.io().out); });
  }
} // namespace ao::cli
