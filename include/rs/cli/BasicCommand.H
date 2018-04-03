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

#include <rs/cli/Command.H>
#include <boost/program_options.hpp>
#include <boost/make_shared.hpp>
#include <functional>

namespace rs::cli
{
  class BasicCommand : public Command
  {
  public:
    using VariablesMap = boost::program_options::variables_map;
    using Executor = std::function<std::error_code(const VariablesMap&)>;

    BasicCommand(const Executor& executor) : _executor{executor} {}

    std::error_code execute(int argc, const char *argv[])
    {
      boost::program_options::command_line_parser parser{argc, argv};
      parser.options(_optDesc).positional(_posOptDesc);
      VariablesMap vm;
      boost::program_options::store(parser.run(), vm);
      boost::program_options::notify(vm);

      return _executor(vm);
    }

    template<typename... Args>
    BasicCommand& addOption(Args&&... args)
    {
      _optDesc.add(boost::make_shared<boost::program_options::option_description>(std::forward<Args>(args)...));
      return *this;
    }

    template<typename... Args>
    BasicCommand& addPostionalOption(Args&&... args)
    {
      _posOptDesc.add(std::forward<Args>(args)...);
      return *this;
    }

  private:
    Executor _executor;
    boost::program_options::options_description _optDesc;
    boost::program_options::positional_options_description _posOptDesc;
  };

}

