// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "ListCommand.h"

#include "DumpUtils.h"
#include <ao/CoreIds.h>
#include <ao/library/ListBuilder.h>
#include <ao/library/ListStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/CoreRuntime.h>

#include <CLI/App.hpp>

#include <cstdint>
#include <iostream>
#include <print>
#include <span>

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

    void createList(library::MusicLibrary& ml,
                    std::string const& name,
                    std::string const& filter,
                    std::string const& desc,
                    ListId parentListId,
                    std::ostream& os)
    {
      auto txn = ml.writeTransaction();

      // Build list payload using ListBuilder
      auto builder = library::ListBuilder::createNew().name(name).description(desc).parentId(parentListId);

      if (!filter.empty())
      {
        builder.filter(filter);
      }

      auto const data = builder.serialize();

      auto createResult = ml.lists().writer(txn).create(data);

      if (!createResult)
      {
        std::println(os, "failed to create list: {}", createResult.error().message);
        return;
      }

      auto const [id, view] = *createResult;

      if (auto result = txn.commit(); !result)
      {
        std::println(os, "failed to commit list: {}", result.error().message);
        return;
      }

      std::println(os, "add list: {} {}", id, name);
    }

    void dumpLists(library::MusicLibrary& ml, bool raw, bool yaml, std::ostream& os)
    {
      auto const txn = ml.readTransaction();
      auto const reader = ml.lists().reader(txn);

      if (yaml)
      {
        std::println(os, "lists:");
      }

      for (auto const& [id, view] : reader)
      {
        if (yaml)
        {
          std::println(os, "  - id: {}", id);
          std::println(os, "    name: \"{}\"", view.name());
          std::println(os, "    description: \"{}\"", view.description());
          std::println(os, "    type: \"{}\"", view.isSmart() ? "smart" : "manual");
          std::println(os, "    parentId: {}", view.parentId());

          if (view.isSmart())
          {
            std::println(os, "    filter: \"{}\"", view.filter());
          }
          else
          {
            std::print(os, "    tracks: [");
            bool first = true;

            for (auto const trackId : view.tracks())
            {
              if (!first)
              {
                std::print(os, ", ");
              }

              std::print(os, "{}", trackId);
              first = false;
            }

            std::println(os, "]");
          }
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
    }
  } // namespace

  void setupListCommand(CLI::App& app, rt::CoreRuntime& runtime)
  {
    auto& ml = runtime.musicLibrary();
    auto* list = app.add_subcommand("list", "List management commands");

    list->add_subcommand("show", "Show all lists")->callback([&ml] { show(ml, std::cout); });

    auto* create = list->add_subcommand("create", "Create a new list");
    auto* name = create->add_option("-n,--name", "list name")->required();
    auto* filter = create->add_option("-f,--filter", "track filter expression");
    auto* desc = create->add_option("-d,--desc", "list description");
    auto* parent = create->add_option("-p,--parent", "parent list id (0 = all-tracks)")->default_val(0);
    create->callback(
      [&ml, name, filter, desc, parent]
      {
        createList(ml,
                   name->as<std::string>(),
                   filter->as<std::string>(),
                   desc->as<std::string>(),
                   ListId{parent->as<std::uint32_t>()},
                   std::cout);
      });

    auto* del = list->add_subcommand("delete", "Delete a list");
    auto* id = del->add_option("id", "list id")->required();
    del->callback(
      [&ml, id]
      {
        auto txn = ml.writeTransaction();
        auto writer = ml.lists().writer(txn);

        auto const listId = ListId{id->as<std::uint32_t>()};

        if (!writer.remove(listId))
        {
          std::println("list not found: {}", listId);
          return;
        }

        std::println("deleted list: {}", listId);

        if (auto result = txn.commit(); !result)
        {
          std::println("failed to commit list delete: {}", result.error().message);
        }
      });

    auto* dumpCmd = list->add_subcommand("dump", "Dump lists from database");
    auto* dumpRaw = dumpCmd->add_flag("--raw", "hex dump raw bytes");
    auto* dumpYaml = dumpCmd->add_flag("--yaml", "output as YAML");

    dumpCmd->callback([&ml, dumpRaw, dumpYaml]
                      { dumpLists(ml, dumpRaw->count() > 0, dumpYaml->count() > 0, std::cout); });
  }
} // namespace ao::cli
