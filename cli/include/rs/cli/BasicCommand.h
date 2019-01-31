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

#include <rs/cli/Command.h>
#include <boost/program_options.hpp>
#include <boost/make_shared.hpp>
#include <functional>

namespace rs::cli
{
  class BasicCommand : public Command
  {
  public:
    using VariablesMap = boost::program_options::variables_map;
    using Executor = std::function<std::string(const VariablesMap&)>;

    BasicCommand()
    {
      addOption("help, h", "help message");
    }

    template<typename... ExecutorArgs>
    BasicCommand(ExecutorArgs&&... args) 
      : _executor{std::forward<ExecutorArgs>(args)...}
    {
      addOption("help, h", "help message");
    }

    std::string execute(int argc, const char *argv[]) override
    {
      boost::program_options::command_line_parser parser{argc, argv};
      parser.options(_optDesc).positional(_posOptDesc);
      VariablesMap vm;
      boost::program_options::store(parser.run(), vm);
      
      if (vm.count("help") > 0)
      {
        std::ostringstream oss;
        oss << _optDesc;
        return oss.str();
      }

      boost::program_options::notify(vm);
      return _executor(vm);
    }

    BasicCommand& addOption(const char* name, const char* description, int positional = 0)
    {
      return addOption(name, new boost::program_options::untyped_value{true}, description, positional);
    }

    BasicCommand& addOption(const char* name, const boost::program_options::value_semantic* s, const char* description, int positional = 0)
    {
      _optDesc.add(boost::make_shared<boost::program_options::option_description>(name, s, description));
      
      if (positional != 0)
      {
        _posOptDesc.add(name, positional);
      }

      return *this;
    }

    BasicCommand& setExecutor(Executor executor)
    {
      _executor = std::move(executor);
      return *this;
    }

  private:
    std::function<std::string(const VariablesMap&)> _executor;
    boost::program_options::options_description _optDesc;
    boost::program_options::positional_options_description _posOptDesc;
  };

}

