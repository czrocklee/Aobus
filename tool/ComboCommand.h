// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "Command.h"
#include <boost/algorithm/string/join.hpp>
#include <boost/program_options.hpp>
#include <boost/range/adaptor/map.hpp>
#include <map>
#include <memory>

// #include <boost/range/adaptor/transformed.hpp>

class ComboCommand : public Command
{
public:
  ComboCommand()
  {
    _optDesc.add_options()("command", boost::program_options::value<std::string>(), "sub command")("help,h", "show help");
    _posOptDesc.add("command", 1);
  }

  template<typename T, typename... Args>
  T& addCommand(std::string cmd, Args&&... args)
  {
    auto ptr = std::make_unique<T>(std::forward<Args>(args)...);
    auto iter = _cmds.emplace(std::move(cmd), std::move(ptr));
    return *(static_cast<T*>(iter.first->second.get()));
  }

  void execute(int argc, char const* argv[], std::ostream& os) override
  {
    std::string_view command = "";

    if (argc > 1)
    {
      command = argv[1];

      // Handle rsc --help or rsc help (root level help)
      if (command == "--help" || command == "help")
      {
        showHelp(os);
        return;
      }

      // Check if this is a valid subcommand
      if (_cmds.find(command) != _cmds.end())
      {
        // Pass remaining arguments to subcommand (which may include --help)
        --argc, ++argv;
        if (auto iter = _cmds.find(command); iter != _cmds.end())
        {
          iter->second->execute(argc, argv, os);
        }
        return;
      }

      // Unknown command - show error with available commands
      std::ostringstream oss;
      oss << "invalid sub command: " << command << "\navailable: ["
          << boost::algorithm::join(boost::adaptors::keys(_cmds), "|") << "]";
      throw std::invalid_argument(oss.str());
    }

    // No command provided, show help
    showHelp(os);
  }

  void showHelp(std::ostream& os) const
  {
    os << "RockStudio CLI - rsc\n\n";
    os << "Usage: rsc <command> [options]\n\n";
    os << "Commands:\n";
    for (auto const& [name, _] : _cmds)
    {
      os << "  " << name << "\n";
    }
    os << "\nOptions:\n";
    os << "  --help, -h     show help\n";
  }

private:
  std::map<std::string, std::unique_ptr<Command>, std::less<>> _cmds;
  boost::program_options::options_description _optDesc;
  boost::program_options::positional_options_description _posOptDesc;
};
