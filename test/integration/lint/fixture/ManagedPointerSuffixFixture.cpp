#include <memory>

namespace Glib
{
  template<typename T>
  class RefPtr
  {
  public:
    RefPtr() = default;
    RefPtr(T* ptr) {}
  };
}

class MyClass
{};

// NEGATIVE
void testFunction(std::shared_ptr<MyClass> validParamPtr)
{
  // POSITIVE
  std::shared_ptr<MyClass> myVar;

  // POSITIVE
  auto myAutoVar = std::make_shared<MyClass>();

  // NEGATIVE
  std::shared_ptr<MyClass> myVarPtr;

  // NEGATIVE
  auto myAutoVarPtr = std::make_shared<MyClass>();

  // POSITIVE
  std::unique_ptr<MyClass> myUnique;

  // NEGATIVE
  std::unique_ptr<MyClass> myUniquePtr;

  // POSITIVE
  std::weak_ptr<MyClass> myWeak;

  // NEGATIVE
  std::weak_ptr<MyClass> myWeakPtr;

  // POSITIVE
  Glib::RefPtr<MyClass> myRef;

  // NEGATIVE
  Glib::RefPtr<MyClass> myRefPtr;

  // NEGATIVE
  MyClass* rawPtr;

  // NEGATIVE
  int justAnInt = 0;
}

// POSITIVE
void testFunctionInvalidParam(std::shared_ptr<MyClass> invalidParam)
{
}

class MyStruct
{
public:
  // POSITIVE
  std::shared_ptr<MyClass> memberVar;

  // NEGATIVE
  std::shared_ptr<MyClass> memberVarPtr;

  // POSITIVE
  std::shared_ptr<MyClass> _privateMember;

  // NEGATIVE
  std::shared_ptr<MyClass> _privateMemberPtr;
};

// NEGATIVE
void testFunctionEmptyParam(std::shared_ptr<MyClass>)
{
  // NEGATIVE
  auto myLambda = [](std::shared_ptr<MyClass>) {};

  // NEGATIVE
  auto myAutoLambda = [](auto, auto, auto) {};
}
