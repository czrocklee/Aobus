
#include <optional>

class TestClass
{
  // POSITIVE
  std::optional<int> _invalidMemberName;

  // NEGATIVE
  std::optional<int> _optValidMemberName;

  // NEGATIVE
  std::optional<int> _fieldWithOptInMiddle;
};

void testOptionalUsage()
{
  // POSITIVE
  [[maybe_unused]] auto noMarker = std::optional<int>{42};

  // NEGATIVE
  [[maybe_unused]] auto optValidName = std::optional<int>{42};

  // NEGATIVE
  [[maybe_unused]] auto hasOptSuffix = std::optional<int>{42};
}
