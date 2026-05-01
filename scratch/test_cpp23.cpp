#include <functional>

int main() {
#if __cpp_lib_move_only_function >= 202110L
    std::move_only_function<void()> f = [](){};
    f();
    return 0;
#else
    #error "std::move_only_function not supported"
#endif
}
