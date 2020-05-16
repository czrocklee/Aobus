/*
 * Copyright (C) <year> <name of author>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of  MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "ListCommand.h"
#include <rs/ml/expr/Parser.h>
#include <rs/ml/expr/Serializer.h>
#include <rs/cli/BasicCommand.h>

#include <iomanip>

namespace
{
  namespace bpo = boost::program_options;
  using namespace rs::ml;

  void show(core::MusicLibrary& ml, core::MusicLibrary::ListId id, std::ostream& os)
  {
    auto cstr = [](const flatbuffers::String* str) { return str == nullptr ? "nil" : str->str(); };
    auto txn = ml.readTransaction();

    if (id != core::MusicLibrary::ListId::invalid())
    {
      auto reader = ml.lists().reader(txn);
      const auto* list = reader[id];
      os << std::setw(5) << id;
      os << std::setw(10) << cstr(list->name()) << std::setw(50) << cstr(list->expr()) << std::setw(50)
         << cstr(list->desc()) << std::endl;
      return;
    }

    for (auto [id, list] : ml.lists().reader(txn))
    {
      os << std::setw(5) << id;
      os << std::setw(10) << cstr(list->name()) << std::setw(50) << cstr(list->expr()) << std::setw(50)
         << cstr(list->desc()) << std::endl;
    }
  }

  void create(core::MusicLibrary& ml,
              const std::string& name,
              const std::string& expr,
              const std::string& desc,
              std::ostream& os)
  {
    auto expression = expr::parse(expr);
    expr::normalize(expression);
    std::string exprStr = expr::serialize(expression);
    auto txn = ml.writeTransaction();
    auto writer = ml.lists().writer(txn);

    auto [id, list] = writer.create([&name, &exprStr, &desc](flatbuffers::FlatBufferBuilder& fbb) {
      return fbs::CreateListDirect(fbb, name.c_str(), exprStr.c_str(), desc.c_str());
    });

    txn.commit();
    show(ml, id, os);
  }

  void del(core::MusicLibrary& ml, core::MusicLibrary::ListId id, std::ostream&)
  {    
    auto txn = ml.writeTransaction();
    ml.lists().writer(txn).del(id);
    txn.commit();
  }
}

namespace rs::rml
{
  ListCommand::ListCommand(ml::core::MusicLibrary& ml) : _ml{ml}
  {
    addCommand<rs::cli::BasicCommand>("show",
                                      [this](const auto& vm, auto& os) { return show(_ml, core::MusicLibrary::ListId::invalid(), os); });

    addCommand<rs::cli::BasicCommand>("create")
      .addOption("name, n", bpo::value<std::string>()->required(), "list name", 1)
      .addOption("filter, f", bpo::value<std::string>()->required(), "track filter expression", 1)
      .addOption("desc, d", bpo::value<std::string>()->default_value(""), "list description", 1)
      .setExecutor([this](const bpo::variables_map& vm, std::ostream& os) {
        create(this->_ml, vm["name"].as<std::string>(), vm["filter"].as<std::string>(), vm["desc"].as<std::string>(), os);
      });

    addCommand<rs::cli::BasicCommand>("delete")
      .addOption("id", bpo::value<std::uint64_t>()->required(), "list id", 1)
      .setExecutor([this](const bpo::variables_map& vm, std::ostream& os) {
        return del(_ml, core::MusicLibrary::ListId{vm["id"].as<std::uint64_t>()}, os);
      });
  }
}