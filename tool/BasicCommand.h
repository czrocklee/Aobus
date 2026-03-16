// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "Command.h"
#include <boost/make_shared.hpp>
#include <boost/program_options.hpp>
#include <functional>

class BasicCommand : public Command
{
public:
  using VariablesMap = boost::program_options::variables_map;
  using Executor = std::function<void(VariablesMap const&, std::ostream& os)>;

  BasicCommand() { addOption("help, h", "help message"); }

  template<typename... ExecutorArgs>
  BasicCommand(ExecutorArgs&&... args) : _executor{std::forward<ExecutorArgs>(args)...}
  {
    addOption("help, h", "help message");
  }

  void execute(int argc, char const* argv[], std::ostream& os) override
  {
    boost::program_options::command_line_parser parser{argc, argv};
    parser.options(_optDesc).positional(_posOptDesc);
    VariablesMap vm;
    boost::program_options::store(parser.run(), vm);

    if (vm.count("help") > 0)
    {
      os << _optDesc;
      return;
    }

    boost::program_options::notify(vm);
    _executor(vm, os);
  }

  BasicCommand& addOption(char const* name, char const* description, int positional = 0)
  {
    return addOption(name, new boost::program_options::untyped_value{true}, description, positional);
  }

  BasicCommand& addOption(char const* name,
                          boost::program_options::value_semantic const* s,
                          char const* description,
                          int positional = 0)
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
  Executor _executor;
  boost::program_options::options_description _optDesc;
  boost::program_options::positional_options_description _posOptDesc;
};
