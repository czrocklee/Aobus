#include <ao/query/Parser.h>
#include <iostream>
#include <string>

// Mock canonicalize
std::string canonicalize(ao::query::Expression const& expr)
{
  return "STUB";
}

int main()
{
  using namespace ao::query;
  std::string q = "$artist = Bach $album = Suite";
  try
  {
    auto expr = parse(q);
    std::cout << "SUCCESS" << std::endl;
  }
  catch (std::exception const& e)
  {
    std::cout << "FAILED: " << e.what() << std::endl;
  }
  return 0;
}
