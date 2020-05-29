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

#include "TrackCommand.h"
#include <rs/ml/expr/Parser.h>
#include <rs/ml/expr/Serializer.h>
#include <rs/ml/expr/Evaluator.h>
#include <rs/cli/BasicCommand.h>
#include <rs/ml/reactive/ItemList.h>
#include <rs/ml/expr/Evaluator.h>

#include <iomanip>

namespace
{
  namespace bpo = boost::program_options;
  using namespace rs::ml;



  void show(core::MusicLibrary& ml, const std::string& filter, std::ostream& os)
  {
    std::ostringstream oss;

    auto cstr = [](const flatbuffers::String* str) { return str == nullptr ? "nil" : str->str(); };
    auto txn = ml.readTransaction();

    if (filter.empty())
    {  expr::Expression root = expr::parse("$AlbumArtist/$Album/($TrackNumber + 12)-$Title");

      for (auto [id, track]: ml.tracks().reader(txn))
      {
      os << std::setw(5) << id << std::get<std::string>(expr::evaluate(root, track)) << std::endl;
        //os << id << " " << // << cstr(track->meta()->artist()) << " " << cstr(track->meta()->title()) << std::endl;
      }
    }
    else
    {
      os << filter << std::endl;
      auto expr = rs::ml::expr::parse(filter);
      
      for (auto [id, track]: ml.tracks().reader(txn))
      {
        if (rs::ml::expr::toBool(rs::ml::expr::evaluate(expr, track)))
        {
          os << id << " " << cstr(track->meta()->artist()) << " " << cstr(track->meta()->title()) << std::endl;
        }
      }
    }
  }

  /*

  std::string create(core::MusicLibrary& ml, const std::string& name, const std::string& expr, const std::string& desc)
  {
    auto expression = query::parse(expr);
    query::normalize(expression);
    std::string exprStr = query::serialize(expression);
    auto txn = ml.lists().writeTransaction();

    auto id = txn.create([&name, &exprStr, &desc] (flatbuffers::FlatBufferBuilder& fbb) 
    {
      return core::CreateTrackDirect(fbb, name.c_str(), exprStr.c_str(), desc.c_str());
    });

    txn.commit();
    return list(ml, id);
  }

  std::string del(core::MusicLibrary& ml, core::TrackId id)
  {
    auto txn = ml.lists().writeTransaction();
    txn.del(id);
    txn.commit();
    return {};
  }*/
 
}



namespace rs::rml
{
  TrackCommand::TrackCommand(ml::core::MusicLibrary& ml) 
    : _ml{ml}
  {
    addCommand<rs::cli::BasicCommand>("show")
      .addOption("filter, f", bpo::value<std::string>()->default_value(""), "track filter expression", 1)
      .setExecutor([this](const auto& vm, auto& os) { return show(_ml, vm["filter"].template as<std::string>(), os); });

    addCommand<rs::cli::BasicCommand>("create")
      .addOption("name, n", bpo::value<std::string>()->required(), "list name", 1)
      .addOption("filter, f", bpo::value<std::string>()->required(), "track filter expression", 1)
      .addOption("desc, d", bpo::value<std::string>(), "list description", 1)
      .setExecutor([this](const auto& vm, auto& os) { return ""; }); // create(this->_ml, vm["name"].as<std::string>(), vm["filter"].as<std::string>(), vm["desc"].as<std::string>()); });

    addCommand<rs::cli::BasicCommand>("delete")
      .addOption("id", bpo::value<std::uint64_t>()->required(), "list id", 1);
 //     .setExecutor([this](const auto& vm) { del(_ml, vm["id"].as<core::TrackId>()); });
  }
}
