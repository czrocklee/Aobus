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

#pragma once

#include <cli/Command.H>
#include <boost/program_options.hpp>

namespace cli
{
  class BasicCommand : public Command
  {
  public:
    using VariablesMap = boost::program_options::variables_map;
    using Executor = std::function<std::error_code(const VariablesMap&)>;

    BasicCommand(const Executor& excutor);

    std::error_code execute(int argc, const char *argv[]) override;

    BasicCommand& addOption(const char* name, const boost::program_options::value_semantic* s, const char* description);

    BasicCommand& addPostionalOption(const char* name, int maxCount);

  private:
    Executor _executor;
    boost::program_options::options_description _optDesc;
    boost::program_options::positional_options_description _posOptDesc;
  };

}

