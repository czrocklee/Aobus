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

  // POSITIVE
  MyClass* rawPtr;

  // NEGATIVE
  MyClass* raw;

  // POSITIVE
  MyClass const* constPtr;

  // NEGATIVE
  int justAnInt = 0;

  // POSITIVE
  void (*callbackPtr)() = nullptr;

  // NEGATIVE
  void (*callback)() = nullptr;

  // POSITIVE
  MyClass* pBuffer;

  // POSITIVE
  void* pVoid;

  // POSITIVE
  std::shared_ptr<MyClass> pManagedPtr;

  // NEGATIVE
  MyClass* preset; // starts with p but followed by lowercase

  // NEGATIVE
  MyClass* p; // single p is not Hungarian notation
}

// POSITIVE
void testFunctionInvalidParam(std::shared_ptr<MyClass> invalidParam)
{
}

// POSITIVE
void testFunctionInvalidUniqueRefParam(std::unique_ptr<MyClass>& invalidUniqueRefParam)
{
}

// POSITIVE
void testFunctionInvalidSharedConstRefParam(std::shared_ptr<MyClass> const& invalidSharedConstRefParam)
{
}

// NEGATIVE
void testFunctionValidManagedRefParam(std::unique_ptr<MyClass>& validManagedRefParamPtr)
{
}

// POSITIVE
void testFunctionRawPtrParam(MyClass* pPtr)
{
}

// POSITIVE
void testFunctionRawPtrRefParam(MyClass*& rawRefPtr)
{
}

// NEGATIVE
void testFunctionRawParam(MyClass* p)
{
}

// NEGATIVE
void testFunctionRawRefParam(MyClass*& rawRef)
{
}

typedef MyClass* MyClassHandle;

// POSITIVE
MyClassHandle handlePtr;

// NEGATIVE
MyClassHandle handle;

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

  // POSITIVE
  MyClass* m_rawPtr;

  // NEGATIVE
  MyClass* m_raw;

  // POSITIVE
  MyClass* _pRow;

  // NEGATIVE
  MyClass* _row;
};

// NEGATIVE
void testFunctionEmptyParam(std::shared_ptr<MyClass>)
{
  // NEGATIVE
  auto myLambda = [](std::shared_ptr<MyClass>) {};

  // POSITIVE
  auto myNamedLambda = [](std::shared_ptr<MyClass> const& namedParam) {};

  // NEGATIVE
  auto myNamedValidLambda = [](std::shared_ptr<MyClass> const& namedParamPtr) {};

  // NEGATIVE
  auto myAutoLambda = [](auto, auto, auto) {};
}

template<typename Callback>
auto guardGenericCallback(Callback callback)
{
  // NEGATIVE: a dependent generic parameter can be instantiated as either a value or a pointer.
  return [callback](auto&&... arguments) { callback(static_cast<decltype(arguments)>(arguments)...); };
}

void instantiateGenericCallback()
{
  auto guarded = guardGenericCallback([](MyClass*) {});
  auto value = MyClass{};
  guarded(&value);

  auto guardedManaged = guardGenericCallback([](std::shared_ptr<MyClass>) {});
  guardedManaged(std::make_shared<MyClass>());
}

template<typename T>
struct GenericHolder
{
  // NEGATIVE: a dependent member can be instantiated as either a value or a pointer.
  T value;
};

GenericHolder<MyClass> valueHolder;
GenericHolder<std::shared_ptr<MyClass>> pointerHolder;
