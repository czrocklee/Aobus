namespace ao
{
  using namespace ao; // Redundant
}

namespace ao::rt
{
  using namespace ao;     // Redundant
  using namespace ao::rt; // Redundant
}

namespace other
{
  using namespace ao; // Not redundant
}

namespace ao
{
  void func()
  {
    using namespace ao; // Redundant
  }
}
