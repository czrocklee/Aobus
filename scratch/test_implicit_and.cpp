#include <ao/query/ExecutionPlan.h>
#include <ao/query/Parser.h>
#include <iostream>

namespace ao::query
{
  // We need to implement these or link them, but for a simple check
  // we can just see if parse() throws.
}

int main()
{
  try
  {
    auto expr = ao::query::parse("$artist = Bach $album = Suite");
    std::cout << "Allowed" << std::endl;
  }
  catch (...)
  {
    std::cout << "Not Allowed" << std::endl;
  }
  return 0;
}
