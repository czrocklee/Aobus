// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ListCommand.h"

#include "CliContext.h"
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
#include <ao/rt/CoreRuntime.h>
#include <ao/rt/ListNode.h>
#include <ao/rt/TrackRow.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryReader.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/rt/source/TrackSource.h>
#include <ao/rt/source/TrackSourceCache.h>
#include <ao/yaml/Reflect.h>

#include <CLI/App.hpp>

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::cli
{
  namespace
  {
    void printListsPlain(library::MusicLibrary& ml, std::ostream& os)
    {
      auto const transaction = ml.readTransaction();
      auto const reader = ml.lists().reader(transaction);

      constexpr int kIdWidth = 5;

      for (auto const& [id, view] : reader)
      {
        std::println(os, "{:>5} {}", id, view.name());
        std::print(os, "{} [{}] parent: ", std::string(kIdWidth, ' '), view.isSmart() ? "smart" : "manual");

        if (view.isRootParent())
        {
          std::println(os, "all-tracks");
        }
        else
        {
          std::println(os, "{}", view.parentId());
        }

        if (view.isSmart())
        {
          std::println(os, "{} [smart] filter: \"{}\"", std::string(kIdWidth, ' '), view.filter());
        }
        else
        {
          std::println(os, "{} [manual] {} tracks", std::string(kIdWidth, ' '), view.tracks().size());
        }

        if (!view.description().empty())
        {
          std::println(os, "{} desc: \"{}\"", std::string(kIdWidth, ' '), view.description());
        }
      }
    }

    std::string_view listKindName(rt::ListNodeKind kind)
    {
      switch (kind)
      {
        case rt::ListNodeKind::Folder: return "folder";
        case rt::ListNodeKind::Manual: return "manual";
        case rt::ListNodeKind::Smart: return "smart";
      }

      return "unknown";
    }
  } // namespace

  struct ListRecordDto final
  {
    ListId id{};
    std::string name{};
    std::string description{};
    std::string type{};
    ListId parentId{};
    std::optional<std::string> optFilter{};
    std::optional<std::vector<TrackId>> optTracks{};
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
    std::string type{};
    ListId parentId{};
    std::optional<std::string> optFilter{};
    std::vector<ListTrackRowDto> tracks{};
  };

  struct ListDetailDocumentDto final
  {
    ListDetailDto list{};
  };
} // namespace ao::cli

template<>
struct ao::yaml::ReflectNameOverrides<ao::cli::ListRecordDto>
{
  static constexpr std::string_view keyFor(std::string_view memberName) noexcept
  {
    if (memberName == "optFilter")
    {
      return "filter";
    }

    if (memberName == "optTracks")
    {
      return "tracks";
    }

    return memberName;
  }
};

template<>
struct ao::yaml::ReflectNameOverrides<ao::cli::ListDetailDto>
{
  static constexpr std::string_view keyFor(std::string_view memberName) noexcept
  {
    if (memberName == "optFilter")
    {
      return "filter";
    }

    return memberName;
  }
};

namespace ao::cli
{
  namespace
  {
    ListRecordDto toListRecordDto(ListId id, library::ListView const& view)
    {
      auto dto = ListRecordDto{.id = id,
                               .name = std::string{view.name()},
                               .description = std::string{view.description()},
                               .type = view.isSmart() ? "smart" : "manual",
                               .parentId = view.parentId()};

      if (view.isSmart())
      {
        dto.optFilter = std::string{view.filter()};
      }
      else
      {
        dto.optTracks = std::vector<TrackId>{view.tracks().begin(), view.tracks().end()};
      }

      return dto;
    }

    void emitListCollectionDocument(library::MusicLibrary& ml, OutputFormat format, std::ostream& os)
    {
      auto const transaction = ml.readTransaction();
      auto const reader = ml.lists().reader(transaction);
      auto dto = ListCollectionDto{};

      for (auto const& [id, view] : reader)
      {
        dto.lists.push_back(toListRecordDto(id, view));
      }

      emitDocument(os, format, dto);
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

    std::vector<rt::TrackRow> resolvedListRows(CliContext& context, rt::ListNode const& node, rt::LibraryReader& reader)
    {
      auto& sources = context.runtime().sources();
      sources.reloadAllTracks();
      auto sourceResult = sources.acquire(node.id);

      if (!sourceResult)
      {
        throwCommandError(sourceResult.error());
      }

      auto sourceLease = std::move(*sourceResult);
      auto& source = sourceLease.source();
      auto ids = std::vector<TrackId>{};
      ids.reserve(source.size());

      for (std::size_t index = 0; index < source.size(); ++index)
      {
        ids.push_back(source.trackIdAt(index));
      }

      return resolveRows(reader, ids);
    }

    void printPlainTrackRows(std::ostream& os, std::vector<rt::TrackRow> const& rows)
    {
      for (auto const& row : rows)
      {
        std::println(os, "    {:>5} {}", row.id, row.title);
      }
    }

    ListTrackRowDto toListTrackRowDto(rt::TrackRow const& row)
    {
      return ListTrackRowDto{.id = row.id, .title = row.title, .artist = row.artist, .album = row.album};
    }

    ListDetailDto toListDetailDto(rt::ListNode const& node, std::vector<rt::TrackRow> const& rows)
    {
      auto dto = ListDetailDto{.id = node.id,
                               .name = node.name,
                               .description = node.description,
                               .type = std::string{listKindName(node.kind)},
                               .parentId = node.parentId};

      if (node.kind == rt::ListNodeKind::Smart)
      {
        dto.optFilter = node.smartExpression;
      }

      dto.tracks.reserve(rows.size());

      for (auto const& row : rows)
      {
        dto.tracks.push_back(toListTrackRowDto(row));
      }

      return dto;
    }

    void printListDetail(CliContext& context, ListId listId)
    {
      auto reader = context.library().reader();
      auto optNode = reader.listNode(listId);

      if (!optNode)
      {
        throwCommandError(Error::Code::NotFound, "list not found: {}", listId);
      }

      auto const& node = *optNode;
      auto const rows = resolvedListRows(context, node, reader);

      if (context.options().format == OutputFormat::Yaml)
      {
        emitDocument(
          context.io().out, context.options().format, ListDetailDocumentDto{.list = toListDetailDto(node, rows)});
        return;
      }

      if (context.options().format == OutputFormat::Json)
      {
        emitDocument(context.io().out, context.options().format, toListDetailDto(node, rows));
        return;
      }

      std::println(context.io().out, "List ID: {}", node.id);
      std::println(context.io().out, "  Name: {}", node.name);
      std::println(context.io().out, "  Description: {}", node.description);
      std::println(context.io().out, "  Type: {}", listKindName(node.kind));
      std::println(context.io().out, "  Parent ID: {}", node.parentId);

      if (node.kind == rt::ListNodeKind::Smart)
      {
        std::println(context.io().out, "  Filter: {}", node.smartExpression);
      }

      std::println(context.io().out, "  Tracks: {}", rows.size());
      printPlainTrackRows(context.io().out, rows);
    }
  } // namespace

  struct ListCreateReportDto final
  {
    std::string action{};
    bool dryRun = false;
    std::optional<ListId> optListId{};
    std::string name{};
    std::string type{};
    ListId parentId{};
    std::optional<std::string> optFilter{};
  };

  struct ListUpdateReportDto final
  {
    std::string action{};
    bool dryRun = false;
    ListId listId{};
    bool changed = false;
    std::vector<rt::ListFieldChange> fields{};
    std::vector<TrackId> addedTrackIds{};
    std::vector<TrackId> removedTrackIds{};
  };

  struct ListManualInsertReportDto final
  {
    std::string action{};
    bool dryRun = false;
    ListId listId{};
    bool changed = false;
    std::size_t insertionIndex = 0;
    std::vector<TrackId> insertedTrackIds{};
    std::vector<TrackId> duplicateRequest{};
    std::vector<TrackId> alreadyPresent{};
    std::vector<TrackId> missingTrack{};
  };

  struct ListManualRemoveReportDto final
  {
    std::string action{};
    bool dryRun = false;
    ListId listId{};
    bool changed = false;
    std::vector<TrackId> removedTrackIds{};
    std::vector<TrackId> duplicateRequest{};
    std::vector<TrackId> notPresent{};
  };

  struct ListDeleteReportDto final
  {
    std::string action{};
    bool dryRun = false;
    ListId listId{};
    std::string name{};
    std::string type{};
    std::uint64_t trackCount = 0;
  };
} // namespace ao::cli

template<>
struct ao::yaml::ReflectNameOverrides<ao::cli::ListCreateReportDto>
{
  static constexpr std::string_view keyFor(std::string_view memberName) noexcept
  {
    if (memberName == "optListId")
    {
      return "listId";
    }

    if (memberName == "optFilter")
    {
      return "filter";
    }

    return memberName;
  }
};

namespace ao::cli
{
  namespace
  {
    std::string draftKindName(rt::LibraryWriter::ListKind kind)
    {
      return kind == rt::LibraryWriter::ListKind::Smart ? "smart" : "manual";
    }

    void printListCreateMutation(CliContext& context,
                                 std::optional<ListId> optListId,
                                 rt::LibraryWriter::ListDraft const& draft,
                                 bool dryRun);

    void createList(CliContext& context,
                    std::string const& name,
                    std::string const& filter,
                    std::string const& desc,
                    ListId parentListId,
                    bool dryRun)
    {
      auto draft = rt::LibraryWriter::ListDraft{};
      draft.kind = filter.empty() ? rt::LibraryWriter::ListKind::Manual : rt::LibraryWriter::ListKind::Smart;
      draft.parentId = parentListId;
      draft.name = name;
      draft.description = desc;
      draft.expression = filter;

      if (dryRun)
      {
        auto const listResult = context.library().writer().previewCreateList(draft);

        if (!listResult)
        {
          throwCommandError(listResult.error());
        }

        printListCreateMutation(context, std::nullopt, draft, true);
        return;
      }

      auto const listResult = context.library().writer().createList(draft);

      if (!listResult)
      {
        throwCommandError(listResult.error());
      }

      printListCreateMutation(context, std::optional<ListId>{*listResult}, draft, false);
    }

    rt::LibraryWriter::ListDraft draftFromNode(rt::LibraryReader& reader, rt::ListNode const& node)
    {
      auto draft = rt::LibraryWriter::ListDraft{};
      draft.kind =
        node.kind == rt::ListNodeKind::Smart ? rt::LibraryWriter::ListKind::Smart : rt::LibraryWriter::ListKind::Manual;
      draft.parentId = node.parentId;
      draft.listId = node.id;
      draft.name = node.name;
      draft.description = node.description;
      draft.expression = node.smartExpression;

      if (draft.kind == rt::LibraryWriter::ListKind::Manual)
      {
        draft.trackIds = reader.listTrackIds(node.id);
      }

      return draft;
    }

    rt::LibraryWriter::ListDraft requireListDraft(ListId listId, rt::LibraryReader& reader)
    {
      auto optNode = reader.listNode(listId);

      if (!optNode)
      {
        throwCommandError(Error::Code::NotFound, "list not found: {}", listId);
      }

      return draftFromNode(reader, *optNode);
    }

    void printListCreateMutation(CliContext& context,
                                 std::optional<ListId> optListId,
                                 rt::LibraryWriter::ListDraft const& draft,
                                 bool dryRun)
    {
      if (context.options().format != OutputFormat::Plain)
      {
        emitDocument(context.io().out,
                     context.options().format,
                     ListCreateReportDto{.action = "create",
                                         .dryRun = dryRun,
                                         .optListId = optListId,
                                         .name = draft.name,
                                         .type = draftKindName(draft.kind),
                                         .parentId = draft.parentId,
                                         .optFilter = draft.kind == rt::LibraryWriter::ListKind::Smart
                                                        ? std::optional{draft.expression}
                                                        : std::nullopt});

        return;
      }

      if (optListId)
      {
        std::println(context.io().out, "add list: {} {}{}", *optListId, draft.name, dryRun ? " (dry-run)" : "");
        return;
      }

      std::println(context.io().out, "add list: {}{}", draft.name, dryRun ? " (dry-run)" : "");
    }

    void printListUpdateMutation(CliContext& context,
                                 std::string_view action,
                                 ListId listId,
                                 rt::UpdateListReply const& reply,
                                 bool dryRun)
    {
      if (context.options().format != OutputFormat::Plain)
      {
        emitDocument(context.io().out,
                     context.options().format,
                     ListUpdateReportDto{.action = std::string{action},
                                         .dryRun = dryRun,
                                         .listId = listId,
                                         .changed = reply.changed,
                                         .fields = reply.fieldChanges,
                                         .addedTrackIds = reply.addedTrackIds,
                                         .removedTrackIds = reply.removedTrackIds});
        return;
      }

      if (action == "add")
      {
        std::println(context.io().out, "added tracks to list: {}{}", listId, dryRun ? " (dry-run)" : "");
      }
      else if (action == "remove")
      {
        std::println(context.io().out, "removed tracks from list: {}{}", listId, dryRun ? " (dry-run)" : "");
      }
      else
      {
        std::println(context.io().out, "updated list: {}{}", listId, dryRun ? " (dry-run)" : "");
      }
    }

    void printListDeleteMutation(CliContext& context, rt::DeleteListReply const& reply, bool dryRun)
    {
      if (context.options().format != OutputFormat::Plain)
      {
        emitDocument(context.io().out,
                     context.options().format,
                     ListDeleteReportDto{.action = "delete",
                                         .dryRun = dryRun,
                                         .listId = reply.listId,
                                         .name = reply.name,
                                         .type = reply.kind,
                                         .trackCount = reply.trackCount});
        return;
      }

      std::println(context.io().out, "deleted list: {}{}", reply.listId, dryRun ? " (dry-run)" : "");
    }

    void printManualListInsertMutation(CliContext& context,
                                       ListId listId,
                                       rt::InsertManualListTracksReply const& reply,
                                       bool dryRun)
    {
      if (context.options().format != OutputFormat::Plain)
      {
        emitDocument(context.io().out,
                     context.options().format,
                     ListManualInsertReportDto{.action = "add",
                                               .dryRun = dryRun,
                                               .listId = listId,
                                               .changed = reply.changed,
                                               .insertionIndex = reply.insertionIndex,
                                               .insertedTrackIds = reply.insertedTrackIds,
                                               .duplicateRequest = reply.duplicateRequest,
                                               .alreadyPresent = reply.alreadyPresent,
                                               .missingTrack = reply.missingTrack});
        return;
      }

      std::println(context.io().out, "added tracks to list: {}{}", listId, dryRun ? " (dry-run)" : "");
    }

    void printManualListRemoveMutation(CliContext& context,
                                       ListId listId,
                                       rt::RemoveManualListTracksReply const& reply,
                                       bool dryRun)
    {
      if (context.options().format != OutputFormat::Plain)
      {
        emitDocument(context.io().out,
                     context.options().format,
                     ListManualRemoveReportDto{.action = "remove",
                                               .dryRun = dryRun,
                                               .listId = listId,
                                               .changed = reply.changed,
                                               .removedTrackIds = reply.removedTrackIds,
                                               .duplicateRequest = reply.duplicateRequest,
                                               .notPresent = reply.notPresent});
        return;
      }

      std::println(context.io().out, "removed tracks from list: {}{}", listId, dryRun ? " (dry-run)" : "");
    }

    void updateList(CliContext& context,
                    ListId listId,
                    std::optional<std::string> const& optName,
                    std::optional<std::string> const& optDescription,
                    std::optional<std::string> const& optFilter,
                    std::optional<std::uint32_t> const& optParent,
                    bool dryRun)
    {
      if (!optName && !optDescription && !optFilter && !optParent)
      {
        throwCommandError(Error::Code::InvalidInput, "list update requires at least one field option");
      }

      auto reader = context.library().reader();
      auto draft = requireListDraft(listId, reader);

      if (optName)
      {
        draft.name = *optName;
      }

      if (optDescription)
      {
        draft.description = *optDescription;
      }

      if (optParent)
      {
        draft.parentId = ListId{*optParent};
      }

      if (optFilter)
      {
        draft.expression = *optFilter;
        draft.kind = optFilter->empty() ? rt::LibraryWriter::ListKind::Manual : rt::LibraryWriter::ListKind::Smart;

        if (draft.kind == rt::LibraryWriter::ListKind::Smart)
        {
          draft.trackIds.clear();
        }
      }

      if (dryRun)
      {
        auto const updateResult = context.library().writer().previewUpdateList(draft);

        if (!updateResult)
        {
          throwCommandError(updateResult.error());
        }

        printListUpdateMutation(context, "update", listId, *updateResult, true);
        return;
      }

      auto const updateResult = context.library().writer().updateList(draft);

      if (!updateResult)
      {
        throwCommandError(updateResult.error());
      }

      printListUpdateMutation(context, "update", listId, *updateResult, false);
    }

    void updateManualMembership(CliContext& context,
                                ListId listId,
                                std::vector<std::uint32_t> const& rawTrackIds,
                                bool add,
                                bool dryRun)
    {
      auto trackIds = std::vector<TrackId>{};
      trackIds.reserve(rawTrackIds.size());

      for (auto const rawTrackId : rawTrackIds)
      {
        trackIds.emplace_back(rawTrackId);
      }

      auto& writer = context.library().writer();

      if (add)
      {
        auto const insertionIndex = [&context, listId]
        {
          auto reader = context.library().reader();
          return reader.listTrackIds(listId).size();
        }();
        auto const insertResult = dryRun ? writer.previewInsertManualListTracks(listId, insertionIndex, trackIds)
                                         : writer.insertManualListTracks(listId, insertionIndex, trackIds);

        if (!insertResult)
        {
          throwCommandError(insertResult.error());
        }

        printManualListInsertMutation(context, listId, *insertResult, dryRun);
        return;
      }

      auto const removeResult = dryRun ? writer.previewRemoveManualListTracks(listId, trackIds)
                                       : writer.removeManualListTracks(listId, trackIds);

      if (!removeResult)
      {
        throwCommandError(removeResult.error());
      }

      printManualListRemoveMutation(context, listId, *removeResult, dryRun);
    }

    void dumpLists(library::MusicLibrary& ml, bool raw, OutputFormat format, std::ostream& os)
    {
      if (raw && format != OutputFormat::Plain)
      {
        throwCommandError(Error::Code::InvalidInput, "list dump --raw supports only plain output");
      }

      auto const transaction = ml.readTransaction();
      auto const reader = ml.lists().reader(transaction);
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
          std::println(os, "  Type: {}", view.isSmart() ? "smart" : "manual");
          std::println(os, "  Parent ID: {}", view.parentId());

          if (view.isSmart())
          {
            std::println(os, "  Filter: {}", view.filter());
          }
          else
          {
            std::println(os, "  Tracks: {}", view.tracks().size());
          }
        }
      }

      if (format != OutputFormat::Plain && !raw)
      {
        emitDocument(os, format, dto);
      }
    }
  } // namespace

  void configureListCommand(CLI::App& app, CliContext& context)
  {
    auto* list = app.add_subcommand("list", "List management commands");
    list->require_subcommand(1);

    auto* showCmd = list->add_subcommand("show", "Show lists");
    auto* showId = showCmd->add_option("id", "list id");
    showCmd->callback(
      [&context, showId]
      {
        if (showId->count() > 0)
        {
          printListDetail(context, ListId{showId->as<std::uint32_t>()});
          return;
        }

        if (context.options().format == OutputFormat::Plain)
        {
          printListsPlain(context.musicLibrary(), context.io().out);
        }
        else
        {
          emitListCollectionDocument(context.musicLibrary(), context.options().format, context.io().out);
        }
      });

    auto* create = list->add_subcommand("create", "Create a new list");
    create->footer(listCreateHelpFooter());
    auto* name = create->add_option("-n,--name", "list name")->required();
    auto* filter = create->add_option("-f,--filter", "track filter expression");
    auto* desc = create->add_option("-d,--desc", "list description");
    auto* parent = create->add_option("-p,--parent", "parent list id (0 = all-tracks)")->default_val(0);
    auto* createDryRun = addDryRunFlag(*create);
    create->callback(
      [&context, name, filter, desc, parent, createDryRun]
      {
        createList(context,
                   name->as<std::string>(),
                   filter->as<std::string>(),
                   desc->as<std::string>(),
                   ListId{parent->as<std::uint32_t>()},
                   isDryRun(createDryRun));
      });

    auto* update = list->add_subcommand("update", "Update a list");
    auto* updateId = update->add_option("id", "list id")->required();
    auto* updateName = update->add_option("--name", "list name");
    auto* updateDesc = update->add_option("--desc", "list description");
    auto* updateFilter = update->add_option("--filter", "smart list filter expression; empty converts to manual");
    auto* updateParent = update->add_option("--parent", "parent list id (0 = all-tracks)");
    auto* updateDryRun = addDryRunFlag(*update);
    update->callback(
      [&context, updateId, updateName, updateDesc, updateFilter, updateParent, updateDryRun]
      {
        auto optName = updateName->count() > 0 ? std::optional{updateName->as<std::string>()} : std::nullopt;
        auto optDesc = updateDesc->count() > 0 ? std::optional{updateDesc->as<std::string>()} : std::nullopt;
        auto optFilter = updateFilter->count() > 0 ? std::optional{updateFilter->as<std::string>()} : std::nullopt;
        auto optParent = updateParent->count() > 0 ? std::optional{updateParent->as<std::uint32_t>()} : std::nullopt;
        updateList(context,
                   ListId{updateId->as<std::uint32_t>()},
                   optName,
                   optDesc,
                   optFilter,
                   optParent,
                   isDryRun(updateDryRun));
      });

    auto* add = list->add_subcommand("add", "Add tracks to a manual list");
    auto* addListId = add->add_option("listId", "list id")->required();
    auto addTrackIdsPtr = std::make_shared<std::vector<std::uint32_t>>();
    add->add_option("trackId", *addTrackIdsPtr, "track id")->required();
    auto* addDryRun = addDryRunFlag(*add);
    add->callback(
      [&context, addListId, addTrackIdsPtr, addDryRun]
      {
        updateManualMembership(
          context, ListId{addListId->as<std::uint32_t>()}, *addTrackIdsPtr, true, isDryRun(addDryRun));
      });

    auto* remove = list->add_subcommand("remove", "Remove tracks from a manual list");
    auto* removeListId = remove->add_option("listId", "list id")->required();
    auto removeTrackIdsPtr = std::make_shared<std::vector<std::uint32_t>>();
    remove->add_option("trackId", *removeTrackIdsPtr, "track id")->required();
    auto* removeDryRun = addDryRunFlag(*remove);
    remove->callback(
      [&context, removeListId, removeTrackIdsPtr, removeDryRun]
      {
        updateManualMembership(
          context, ListId{removeListId->as<std::uint32_t>()}, *removeTrackIdsPtr, false, isDryRun(removeDryRun));
      });

    auto* del = list->add_subcommand("delete", "Delete a list");
    auto* id = del->add_option("id", "list id")->required();
    auto* deleteDryRun = addDryRunFlag(*del);
    del->callback(
      [&context, id, deleteDryRun]
      {
        auto const listId = ListId{id->as<std::uint32_t>()};

        if (isDryRun(deleteDryRun))
        {
          auto const deleteResult = context.library().writer().previewDeleteList(listId);

          if (!deleteResult)
          {
            throwCommandError(deleteResult.error());
          }

          printListDeleteMutation(context, *deleteResult, true);
          return;
        }

        auto const deleteResult = context.library().writer().deleteList(listId);

        if (!deleteResult)
        {
          throwCommandError(deleteResult.error());
        }

        printListDeleteMutation(context, *deleteResult, false);
      });

    auto* dumpCmd = list->add_subcommand("dump", "Dump lists from database");
    auto* dumpRaw = dumpCmd->add_flag("--raw", "hex dump raw bytes");

    dumpCmd->callback(
      [&context, dumpRaw]
      { dumpLists(context.musicLibrary(), dumpRaw->count() > 0, context.options().format, context.io().out); });
  }
} // namespace ao::cli
