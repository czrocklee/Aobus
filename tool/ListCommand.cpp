/*
 * Copyright (C) 2025 RockStudio
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "ListCommand.h"
#include <rs/core/ListLayout.h>

#include "BasicCommand.h"

#include <iomanip>

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
      os << std::setw(5) << static_cast<std::uint64_t>(id) << " " << view.name() << std::endl;
    }
  }
}

ListCommand::ListCommand(core::MusicLibrary& ml) : _ml{ml}
{
  addCommand<BasicCommand>("show").setExecutor([this](const auto& vm, auto& os) { return show(_ml, os); });

  addCommand<BasicCommand>("create")
    .addOption("name, n", bpo::value<std::string>()->required(), "list name", 1)
    .addOption("filter, f", bpo::value<std::string>()->required(), "track filter expression", 1)
    .addOption("desc, d", bpo::value<std::string>(), "list description", 1)
    .setExecutor([this](const auto& vm, auto& os) { return ""; });

  addCommand<BasicCommand>("delete")
    .addOption("id", bpo::value<std::uint64_t>()->required(), "list id", 1)
    .setExecutor([this](const auto& vm, auto& os) {
      auto id = core::ListStore::Id{vm["id"].template as<std::uint64_t>()};
      auto txn = _ml.writeTransaction();
      auto writer = _ml.lists().writer(txn);
      if (writer.del(id))
      {
        os << "deleted list: " << static_cast<std::uint64_t>(id) << std::endl;
        txn.commit();
      }
      else
      {
        os << "list not found: " << static_cast<std::uint64_t>(id) << std::endl;
      }
      return "";
    });
}
