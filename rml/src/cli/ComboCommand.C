
#include <cli/ComboCommand.H>

namespace po = boost::program_options;

namespace cli
{
  ComboCommand::ComboCommand()
  {
    _optDesc.add_options()("command", po::value<std::string>()->required(), "sub command");
    _posOptDesc.add("command", 1);
  }

  std::error_code ComboCommand::execute(int argc, const char *argv[])
  {
    std::string command = argv[0];
    auto iter = _cmds.find(command);

    if (iter != _cmds.end())
    {
      return iter->second->execute(argc - 1, argv + 1);
    }
    else
    {
      return std::make_error_code(std::errc::invalid_argument);
    }
  }
}
