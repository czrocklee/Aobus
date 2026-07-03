// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ListCommand.h"

#include "CliContext.h"
#include "CommandError.h"
#include "DumpUtils.h"
#include "Output.h"
#include "TrackSelection.h"
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
#include <ao/rt/source/ListSourceStore.h>
#include <ao/rt/source/TrackSource.h>

#include <CLI/App.hpp>

#include <algorithm>
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
    void show(library::MusicLibrary& ml, std::ostream& os)
    {
      auto const txn = ml.readTransaction();
      auto const reader = ml.lists().reader(txn);

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

    void yamlTrackIds(std::ostream& os, library::ListView const& view)
    {
      if (view.tracks().empty())
      {
        std::println(os, "    tracks: []");
        return;
      }

      std::println(os, "    tracks:");

      for (auto const trackId : view.tracks())
      {
        std::println(os, "      - {}", trackId);
      }
    }

    void jsonTrackIds(std::ostream& os, library::ListView const& view)
    {
      auto array = JsonArray{os};

      for (auto const trackId : view.tracks())
      {
        array.element();
        std::print(os, "{}", trackId.raw());
      }
    }

    void formatYamlListRecord(std::ostream& os, ListId id, library::ListView const& view)
    {
      std::println(os, "  - id: {}", id);
      yamlKeyValue(os, 4, "name", view.name());
      yamlKeyValue(os, 4, "description", view.description());
      yamlKeyValue(os, 4, "type", view.isSmart() ? std::string_view{"smart"} : std::string_view{"manual"});
      yamlKeyValue(os, 4, "parentId", static_cast<std::uint64_t>(view.parentId().raw()));

      if (view.isSmart())
      {
        yamlKeyValue(os, 4, "filter", view.filter());
      }
      else
      {
        yamlTrackIds(os, view);
      }
    }

    void formatJsonListRecord(std::ostream& os, ListId id, library::ListView const& view)
    {
      auto object = JsonObject{os};
      object.uintField("id", id.raw());
      object.stringField("name", view.name());
      object.stringField("description", view.description());
      object.stringField("type", view.isSmart() ? std::string_view{"smart"} : std::string_view{"manual"});
      object.uintField("parentId", view.parentId().raw());

      if (view.isSmart())
      {
        object.stringField("filter", view.filter());
      }
      else
      {
        object.field("tracks");
        jsonTrackIds(os, view);
      }

      object.close();
      std::println(os);
    }

    void showStructured(library::MusicLibrary& ml, OutputFormat format, std::ostream& os)
    {
      auto const txn = ml.readTransaction();
      auto const reader = ml.lists().reader(txn);
      bool hasRows = false;

      for (auto const& [id, view] : reader)
      {
        if (format == OutputFormat::Yaml)
        {
          if (!hasRows)
          {
            std::println(os, "lists:");
          }

          formatYamlListRecord(os, id, view);
        }
        else
        {
          formatJsonListRecord(os, id, view);
        }

        hasRows = true;
      }

      if (format == OutputFormat::Yaml && !hasRows)
      {
        std::println(os, "lists: []");
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
      auto& source = sources.sourceFor(node.id);
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

    void printYamlTrackRows(std::ostream& os, std::vector<rt::TrackRow> const& rows)
    {
      if (rows.empty())
      {
        std::println(os, "  tracks: []");
        return;
      }

      std::println(os, "  tracks:");

      for (auto const& row : rows)
      {
        std::println(os, "    - id: {}", row.id.raw());
        std::int32_t constexpr kIndent = 6;
        yamlKeyValue(os, kIndent, "title", row.title);
        yamlKeyValue(os, kIndent, "artist", row.artist);
        yamlKeyValue(os, kIndent, "album", row.album);
      }
    }

    void printJsonTrackRows(std::ostream& os, std::vector<rt::TrackRow> const& rows)
    {
      auto array = JsonArray{os};

      for (auto const& row : rows)
      {
        array.element();
        auto object = JsonObject{os};
        object.uintField("id", row.id.raw());
        object.stringField("title", row.title);
        object.stringField("artist", row.artist);
        object.stringField("album", row.album);
      }
    }

    void showListDetail(CliContext& context, ListId listId)
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
        std::println(context.io().out, "list:");
        yamlKeyValue(context.io().out, 2, "id", static_cast<std::uint64_t>(node.id.raw()));
        yamlKeyValue(context.io().out, 2, "name", node.name);
        yamlKeyValue(context.io().out, 2, "description", node.description);
        yamlKeyValue(context.io().out, 2, "type", listKindName(node.kind));
        yamlKeyValue(context.io().out, 2, "parentId", static_cast<std::uint64_t>(node.parentId.raw()));

        if (node.kind == rt::ListNodeKind::Smart)
        {
          yamlKeyValue(context.io().out, 2, "filter", node.smartExpression);
        }

        printYamlTrackRows(context.io().out, rows);
        return;
      }

      if (context.options().format == OutputFormat::Json)
      {
        auto object = JsonObject{context.io().out};
        object.uintField("id", node.id.raw());
        object.stringField("name", node.name);
        object.stringField("description", node.description);
        object.stringField("type", listKindName(node.kind));
        object.uintField("parentId", node.parentId.raw());

        if (node.kind == rt::ListNodeKind::Smart)
        {
          object.stringField("filter", node.smartExpression);
        }

        object.field("tracks");
        printJsonTrackRows(context.io().out, rows);
        object.close();
        std::println(context.io().out);
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

    void printListMutation(CliContext& context, std::string_view action, ListId listId, std::string_view name);

    void createList(CliContext& context,
                    std::string const& name,
                    std::string const& filter,
                    std::string const& desc,
                    ListId parentListId)
    {
      auto draft = rt::LibraryWriter::ListDraft{};
      draft.kind = filter.empty() ? rt::LibraryWriter::ListKind::Manual : rt::LibraryWriter::ListKind::Smart;
      draft.parentId = parentListId;
      draft.name = name;
      draft.description = desc;
      draft.expression = filter;

      auto const listResult = context.library().writer().createList(draft);

      if (!listResult)
      {
        throwCommandError(listResult.error());
      }

      auto const listId = *listResult;
      printListMutation(context, "create", listId, name);
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

    void printListMutation(CliContext& context, std::string_view action, ListId listId, std::string_view name = {})
    {
      if (context.options().format == OutputFormat::Yaml)
      {
        yamlKeyValue(context.io().out, 0, "action", action);
        yamlKeyValue(context.io().out, 0, "listId", static_cast<std::uint64_t>(listId.raw()));

        if (!name.empty())
        {
          yamlKeyValue(context.io().out, 0, "name", name);
        }

        return;
      }

      if (context.options().format == OutputFormat::Json)
      {
        auto object = JsonObject{context.io().out};
        object.stringField("action", action);
        object.uintField("listId", listId.raw());

        if (!name.empty())
        {
          object.stringField("name", name);
        }

        object.close();
        std::println(context.io().out);
        return;
      }

      if (action == "create")
      {
        std::println(context.io().out, "add list: {} {}", listId, name);
      }
      else if (action == "delete")
      {
        std::println(context.io().out, "deleted list: {}", listId);
      }
      else if (action == "add")
      {
        std::println(context.io().out, "added tracks to list: {}", listId);
      }
      else if (action == "remove")
      {
        std::println(context.io().out, "removed tracks from list: {}", listId);
      }
      else
      {
        std::println(context.io().out, "updated list: {}", listId);
      }
    }

    void updateList(CliContext& context,
                    ListId listId,
                    std::optional<std::string> const& optName,
                    std::optional<std::string> const& optDescription,
                    std::optional<std::string> const& optFilter,
                    std::optional<std::uint32_t> const& optParent)
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

      auto const updateResult = context.library().writer().updateList(draft);

      if (!updateResult)
      {
        throwCommandError(updateResult.error());
      }

      printListMutation(context, "update", listId);
    }

    void updateManualMembership(CliContext& context,
                                ListId listId,
                                std::vector<std::uint32_t> const& rawTrackIds,
                                bool add)
    {
      auto reader = context.library().reader();
      auto draft = requireListDraft(listId, reader);

      if (draft.kind != rt::LibraryWriter::ListKind::Manual)
      {
        throwCommandError(Error::Code::InvalidInput, "list is not manual: {}", listId);
      }

      auto const trackIds = requireTrackIds(reader, rawTrackIds);

      for (auto const trackId : trackIds)
      {
        if (add)
        {
          if (!std::ranges::contains(draft.trackIds, trackId))
          {
            draft.trackIds.push_back(trackId);
          }
        }
        else
        {
          std::erase(draft.trackIds, trackId);
        }
      }

      auto const updateResult = context.library().writer().updateList(draft);

      if (!updateResult)
      {
        throwCommandError(updateResult.error());
      }

      printListMutation(context, add ? "add" : "remove", listId);
    }

    void dumpLists(library::MusicLibrary& ml, bool raw, OutputFormat format, std::ostream& os)
    {
      if (raw && format != OutputFormat::Plain)
      {
        throwCommandError(Error::Code::InvalidInput, "list dump --raw supports only plain output");
      }

      auto const txn = ml.readTransaction();
      auto const reader = ml.lists().reader(txn);
      bool hasRows = false;

      for (auto const& [id, view] : reader)
      {
        if (format == OutputFormat::Yaml && !raw)
        {
          if (!hasRows)
          {
            std::println(os, "lists:");
          }

          formatYamlListRecord(os, id, view);
        }
        else if (format == OutputFormat::Json && !raw)
        {
          formatJsonListRecord(os, id, view);
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

        hasRows = true;
      }

      if (format == OutputFormat::Yaml && !raw && !hasRows)
      {
        std::println(os, "lists: []");
      }
    }
  } // namespace

  void setupListCommand(CLI::App& app, CliContext& context)
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
          showListDetail(context, ListId{showId->as<std::uint32_t>()});
          return;
        }

        if (context.options().format == OutputFormat::Plain)
        {
          show(context.musicLibrary(), context.io().out);
        }
        else
        {
          showStructured(context.musicLibrary(), context.options().format, context.io().out);
        }
      });

    auto* create = list->add_subcommand("create", "Create a new list");
    auto* name = create->add_option("-n,--name", "list name")->required();
    auto* filter = create->add_option("-f,--filter", "track filter expression");
    auto* desc = create->add_option("-d,--desc", "list description");
    auto* parent = create->add_option("-p,--parent", "parent list id (0 = all-tracks)")->default_val(0);
    create->callback(
      [&context, name, filter, desc, parent]
      {
        createList(context,
                   name->as<std::string>(),
                   filter->as<std::string>(),
                   desc->as<std::string>(),
                   ListId{parent->as<std::uint32_t>()});
      });

    auto* update = list->add_subcommand("update", "Update a list");
    auto* updateId = update->add_option("id", "list id")->required();
    auto* updateName = update->add_option("--name", "list name");
    auto* updateDesc = update->add_option("--desc", "list description");
    auto* updateFilter = update->add_option("--filter", "smart list filter expression; empty converts to manual");
    auto* updateParent = update->add_option("--parent", "parent list id (0 = all-tracks)");
    update->callback(
      [&context, updateId, updateName, updateDesc, updateFilter, updateParent]
      {
        auto optName = updateName->count() > 0 ? std::optional{updateName->as<std::string>()} : std::nullopt;
        auto optDesc = updateDesc->count() > 0 ? std::optional{updateDesc->as<std::string>()} : std::nullopt;
        auto optFilter = updateFilter->count() > 0 ? std::optional{updateFilter->as<std::string>()} : std::nullopt;
        auto optParent = updateParent->count() > 0 ? std::optional{updateParent->as<std::uint32_t>()} : std::nullopt;
        updateList(context, ListId{updateId->as<std::uint32_t>()}, optName, optDesc, optFilter, optParent);
      });

    auto* add = list->add_subcommand("add", "Add tracks to a manual list");
    auto* addListId = add->add_option("listId", "list id")->required();
    auto addTrackIdsPtr = std::make_shared<std::vector<std::uint32_t>>();
    add->add_option("trackId", *addTrackIdsPtr, "track id")->required();
    add->callback([&context, addListId, addTrackIdsPtr]
                  { updateManualMembership(context, ListId{addListId->as<std::uint32_t>()}, *addTrackIdsPtr, true); });

    auto* remove = list->add_subcommand("remove", "Remove tracks from a manual list");
    auto* removeListId = remove->add_option("listId", "list id")->required();
    auto removeTrackIdsPtr = std::make_shared<std::vector<std::uint32_t>>();
    remove->add_option("trackId", *removeTrackIdsPtr, "track id")->required();
    remove->callback(
      [&context, removeListId, removeTrackIdsPtr]
      { updateManualMembership(context, ListId{removeListId->as<std::uint32_t>()}, *removeTrackIdsPtr, false); });

    auto* del = list->add_subcommand("delete", "Delete a list");
    auto* id = del->add_option("id", "list id")->required();
    del->callback(
      [&context, id]
      {
        auto const listId = ListId{id->as<std::uint32_t>()};

        if (!context.library().writer().deleteList(listId))
        {
          throwCommandError(Error::Code::NotFound, "list not found: {}", listId);
        }

        printListMutation(context, "delete", listId);
      });

    auto* dumpCmd = list->add_subcommand("dump", "Dump lists from database");
    auto* dumpRaw = dumpCmd->add_flag("--raw", "hex dump raw bytes");

    dumpCmd->callback(
      [&context, dumpRaw]
      { dumpLists(context.musicLibrary(), dumpRaw->count() > 0, context.options().format, context.io().out); });
  }
} // namespace ao::cli
