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

#include "TrackCommand.h"
#include "BasicCommand.h"
#include <rs/core/TrackLayout.h>
#include <rs/expr/ExecutionPlan.h>
#include <rs/expr/Parser.h>
#include <rs/expr/PlanEvaluator.h>

#include <iomanip>

namespace
{
  namespace bpo = boost::program_options;
  using namespace rs;

  void show(core::MusicLibrary& ml, std::string const& filter, std::ostream& os)
  {
    auto txn = ml.readTransaction();
    auto reader = ml.tracks().reader(txn);

    if (filter.empty())
    {
      for (auto [id, view] : reader)
      {
        os << std::setw(5) << id << " " << view.metadata().title() << std::endl;
      }
    }
    else
    {
      auto expr = rs::expr::parse(filter);
      rs::expr::QueryCompiler compiler;
      auto plan = compiler.compile(expr);
      rs::expr::PlanEvaluator evaluator;

      for (auto [id, view] : reader)
      {
        if (evaluator.evaluateFull(plan, view))
        {
          os << std::setw(5) << id << " " << view.metadata().title() << std::endl;
        }
      }
    }
  }
}

TrackCommand::TrackCommand(core::MusicLibrary& ml) : _ml{ml}
{
  addCommand<BasicCommand>("show")
    .addOption("filter, f", bpo::value<std::string>()->default_value(""), "track filter expression", 1)
    .setExecutor([this](auto const& vm, auto& os) { return show(_ml, vm["filter"].template as<std::string>(), os); });

  addCommand<BasicCommand>("create")
    .addOption("name, n", bpo::value<std::string>()->required(), "list name", 1)
    .addOption("filter, f", bpo::value<std::string>()->required(), "track filter expression", 1)
    .addOption("desc, d", bpo::value<std::string>(), "list description", 1)
    .setExecutor([this](auto const& vm, auto& os) { (void) vm; (void) os; return ""; });

  addCommand<BasicCommand>("delete").addOption("id", bpo::value<std::uint64_t>()->required(), "list id", 1);
}
