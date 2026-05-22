#include "TestHelpers.h"

// NEGATIVE: Correct order (public -> protected -> private)
class CorrectOrderDemo
{
public:
  void doPublic();

protected:
  void doProtected();

private:
  int _val1;
};

class OrderDemo
{
private:
  int _val1;

  // POSITIVE
public:
  void doPublic();
};

class StructOrderDemo
{
protected:
  int val1;

  // POSITIVE
public:
  void doPublic();
};
