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
#include <map>
#include <memory>
#include <boost/program_options.hpp>
#include <boost/algorithm/string/join.hpp>
#include <boost/range/adaptor/map.hpp>

//#include <boost/range/adaptor/transformed.hpp>

namespace rs::cli
{
  class ComboCommand : public Command
  {
  public:
    ComboCommand()
    {
      _optDesc.add_options()("command", boost::program_options::value<std::string>(), "sub command");
      _posOptDesc.add("command", 1);
    }

    template<typename T, typename... Args>
    T& addCommand(std::string cmd, Args&&... args)
    {
      auto ptr = std::make_unique<T>(std::forward<Args>(args)...);
      auto iter = _cmds.emplace(std::move(cmd), std::move(ptr));
      return *(static_cast<T*>(iter.first->second.get()));
    }

    std::string execute(int argc, const char *argv[]) override
    {
      std::string_view command = "";
      
      if (argc > 1)
      {
        command = argv[1];
        --argc, ++argv;
      }

      auto iter = _cmds.find(command);

      if (iter != _cmds.end())
      {
        return iter->second->execute(argc, argv);
      }
      else
      {
        std::ostringstream oss;
        oss << "invalid sub command " << command << "available: ["
            << boost::algorithm::join(boost::adaptors::keys(_cmds), "|")
            << "]";
        throw std::invalid_argument(oss.str());
      }
    }

  private:
    std::map<std::string, std::unique_ptr<Command>, std::less<>> _cmds;
    boost::program_options::options_description _optDesc;
    boost::program_options::positional_options_description _posOptDesc;
  };
}


