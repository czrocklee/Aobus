// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "ListCommand.h"
#include <ao/library/ListBuilder.h>
#include <ao/library/ListStore.h>

#include <iomanip>
#include <span>
#include <vector>

namespace ao::cli
{
  namespace
  {
    void show(ao::library::MusicLibrary& ml, std::ostream& os)
    {
      auto const txn = ml.readTransaction();
      auto const reader = ml.lists().reader(txn);

      constexpr int idWidth = 5;
      for (auto const& [id, view] : reader)
      {
        os << std::setw(idWidth) << id << " " << view.name() << "\n";
        os << std::string(idWidth, ' ') << "  [" << (view.isSmart() ? "smart" : "manual") << "] parent: ";

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
          os << std::string(idWidth, ' ') << "  [smart] filter: \"" << view.filter() << "\"\n";
        }
        else
        {
          os << std::string(idWidth, ' ') << "  [manual] " << view.tracks().size() << " tracks\n";
        }

        if (!view.description().empty())
        {
          os << std::string(idWidth, ' ') << "  desc: \"" << view.description() << "\"\n";
        }
      }
    }

    void createList(ao::library::MusicLibrary& ml,
                    std::string const& name,
                    std::string const& filter,
                    std::string const& desc,
                    ListId parentListId,
                    std::ostream& os)
    {
      auto txn = ml.writeTransaction();

      // Build list payload using ListBuilder
      auto builder = ao::library::ListBuilder::createNew().name(name).description(desc).parentId(parentListId);

      if (!filter.empty())
      {
        builder.filter(filter);
      }

      auto const data = builder.serialize();

      auto const [id, view] = ml.lists().writer(txn).create(data);
      txn.commit();

      os << "add list: " << id << " " << name << "\n";
    }
  }

  void setupListCommand(CLI::App& app, ao::library::MusicLibrary& ml)
  {
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

        if (writer.del(listId))
        {
          std::cout << "deleted list: " << listId << "\n";
          txn.commit();
        }
        else
        {
          std::cout << "list not found: " << listId << "\n";
        }
      });
  }
}
