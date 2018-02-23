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
#include <map>
#include <memory>
#include <boost/program_options.hpp>

namespace cli
{
  class ComboCommand : public Command
  {
  public:
    ComboCommand();

    template<typename T, typename... Args>
    T& addCommand(std::string cmd, Args&&... args)
    {
      auto ptr = std::make_unique<T>(std::forward<Args>(args)...);
      auto iter = _cmds.emplace(std::move(cmd), std::move(ptr));
      return *(static_cast<T*>(iter.first->second.get()));
    }

    std::error_code execute(int argc, const char *argv[]) override;

  private:
    std::map<std::string, std::unique_ptr<Command>> _cmds;
    boost::program_options::options_description _optDesc;
    boost::program_options::positional_options_description _posOptDesc;
  };
}





