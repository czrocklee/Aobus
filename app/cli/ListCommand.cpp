// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "ListCommand.h"

#include "DumpUtils.h"
#include "ao/Type.h"
#include "ao/library/ListBuilder.h"
#include "ao/library/ListStore.h"
#include "ao/library/MusicLibrary.h"
#include "runtime/CoreRuntime.h"

#include <CLI/App.hpp>

#include <cstdint>
#include <iomanip>
#include <iostream>
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
        os << std::setw(kIdWidth) << id << " " << view.name() << "\n";
        os << std::string(kIdWidth, ' ') << " [" << (view.isSmart() ? "smart" : "manual") << "] parent: ";

        if (view.isRootParent())
        {
          os << "all-tracks\n";
        }
        else
        {
          os << view.parentId() << "\n";
        }

        if (view.isSmart())
        {
          os << std::string(kIdWidth, ' ') << " [smart] filter: \"" << view.filter() << "\"\n";
        }
        else
        {
          os << std::string(kIdWidth, ' ') << " [manual] " << view.tracks().size() << " tracks\n";
        }

        if (!view.description().empty())
        {
          os << std::string(kIdWidth, ' ') << " desc: \"" << view.description() << "\"\n";
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

      auto const [id, view] = ml.lists().writer(txn).create(data);
      txn.commit();

      os << "add list: " << id << " " << name << "\n";
    }

    void dumpLists(library::MusicLibrary& ml, bool raw, bool yaml, std::ostream& os)
    {
      auto const txn = ml.readTransaction();
      auto const reader = ml.lists().reader(txn);

      if (yaml)
      {
        os << "lists:\n";
      }

      for (auto const& [id, view] : reader)
      {
        if (yaml)
        {
          os << "  - id: " << id << "\n"
             << "    name: \"" << view.name() << "\"\n"
             << "    description: \"" << view.description() << "\"\n"
             << "    type: \"" << (view.isSmart() ? "smart" : "manual") << "\"\n"
             << "    parentId: " << view.parentId() << "\n";

          if (view.isSmart())
          {
            os << "    filter: \"" << view.filter() << "\"\n";
          }
          else
          {
            os << "    tracks: [";
            bool first = true;

            for (auto const trackId : view.tracks())
            {
              if (!first)
              {
                os << ", ";
              }

              os << trackId;
              first = false;
            }

            os << "]\n";
          }
        }
        else if (raw)
        {
          os << "List ID: " << id << "\n";
          hexDump(view.rawData(), os);
        }
        else
        {
          os << "List ID: " << id << "\n"
             << "  Name: " << view.name() << "\n"
             << "  Description: " << view.description() << "\n"
             << "  Type: " << (view.isSmart() ? "smart" : "manual") << "\n"
             << "  Parent ID: " << view.parentId() << "\n";

          if (view.isSmart())
          {
            os << "  Filter: " << view.filter() << "\n";
          }
          else
          {
            os << "  Tracks: " << view.tracks().size() << "\n";
          }
        }
      }
    }
  }

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

        if (auto const listId = ListId{id->as<std::uint32_t>()}; writer.del(listId))
        {
          std::cout << "deleted list: " << listId << "\n";
          txn.commit();
        }
        else
        {
          std::cout << "list not found: " << listId << "\n";
        }
      });

    auto* dumpCmd = list->add_subcommand("dump", "Dump lists from database");
    auto* dumpRaw = dumpCmd->add_flag("--raw", "hex dump raw bytes");
    auto* dumpYaml = dumpCmd->add_flag("--yaml", "output as YAML");

    dumpCmd->callback([&ml, dumpRaw, dumpYaml]
                      { dumpLists(ml, dumpRaw->count() > 0, dumpYaml->count() > 0, std::cout); });
  }
}
