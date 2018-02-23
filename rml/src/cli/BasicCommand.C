/*
 * BasicCommand.C
 *
 *  Created on: Mar 14, 2017
 *      Author: rocklee
 */

#include <cli/BasicCommand.H>
#include <boost/make_shared.hpp>

namespace po = boost::program_options;

namespace cli
{
  BasicCommand::BasicCommand(const Executor& executor)
    : _executor{executor}
  {
  }

  std::error_code BasicCommand::execute(int argc, const char *argv[])
  {
    po::command_line_parser parser{argc, argv};
    parser.options(_optDesc).positional(_posOptDesc);
    VariablesMap vm;
    po::store(parser.run(), vm);
    po::notify(vm);

    return _executor(vm);
  }

  BasicCommand& BasicCommand::addOption(const char* name, const po::value_semantic* s, const char* description)
  {
    _optDesc.add(boost::make_shared<po::option_description>(name, s, description));
    return *this;
  }

  BasicCommand& BasicCommand::addPostionalOption(const char* name, int maxCount)
  {
    _posOptDesc.add(name, maxCount);
    return *this;
  }
}
