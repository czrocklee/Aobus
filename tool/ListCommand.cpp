// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "ListCommand.h"
#include <rs/core/ListLayout.h>
#include <rs/core/ListStore.h>
#include <rs/core/Dictionary.h>

#include "BasicCommand.h"

#include <cstring>
#include <iomanip>
#include <span>
#include <vector>

namespace
{
  namespace bpo = boost::program_options;
  using namespace rs;

  void show(core::MusicLibrary& ml, std::ostream& os)
  {
    auto txn = ml.readTransaction();
    auto reader = ml.lists().reader(txn);

    for (auto [id, view] : reader)
    {
      os << std::setw(5) << id << " " << view.name() << std::endl;
    }
  }

  void createList(core::MusicLibrary& ml, std::string const& name, std::string const& filter, std::string const& desc, std::ostream& os)
  {
    auto txn = ml.writeTransaction();
    auto dict = ml.dictionary();

    // Store strings in dictionary and get IDs
    auto nameId = dict.put(txn, name);
    auto filterId = dict.put(txn, filter);
    auto descId = dict.put(txn, desc);

    // Build list header
    core::ListHeader header{};
    header.nameId = nameId.value();
    header.filterId = filterId.value();
    header.descId = descId.value();
    header.trackIdsCount = 0;
    header.flags = filter.empty() ? 0 : 1; // 0 = manual, 1 = smart (has filter)

    // Serialize header
    std::vector<std::byte> data(sizeof(core::ListHeader));
    std::memcpy(data.data(), &header, sizeof(core::ListHeader));

    auto [id, view] = ml.lists().writer(txn).create(data);
    txn.commit();

    os << "add list: " << id << " " << name << std::endl;
  }
}

ListCommand::ListCommand(core::MusicLibrary& ml) : _ml{ml}
{
  addCommand<BasicCommand>("show").setExecutor([this](auto const& vm, auto& os) {
    (void)vm;
    return show(_ml, os);
  });

  addCommand<BasicCommand>("create")
    .addOption("name,n", bpo::value<std::string>()->required(), "list name", 1)
    .addOption("filter,f", bpo::value<std::string>()->default_value(""), "track filter expression", 1)
    .addOption("desc,d", bpo::value<std::string>()->default_value(""), "list description", 1)
    .setExecutor([this](auto const& vm, auto& os) {
      auto name = vm["name"].template as<std::string>();
      auto filter = vm["filter"].template as<std::string>();
      auto desc = vm["desc"].template as<std::string>();
      createList(_ml, name, filter, desc, os);
      return "";
    });

  addCommand<BasicCommand>("delete")
    .addOption("id", bpo::value<std::uint64_t>()->required(), "list id", 1)
    .setExecutor([this](auto const& vm, auto& os) {
      auto id = core::ListStore::Id{vm["id"].template as<std::uint32_t>()};
      auto txn = _ml.writeTransaction();
      auto writer = _ml.lists().writer(txn);
      if (writer.del(id))
      {
        os << "deleted list: " << id << std::endl;
        txn.commit();
      }
      else
      {
        os << "list not found: " << id << std::endl;
      }
      return "";
    });
}
