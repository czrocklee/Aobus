# Custom Clang-Tidy Checks

This directory contains custom clang-tidy checks for the Aobus project.

## `EmptyLineBeforeIfCheck`

A custom readability check that enforces an empty line before `if` statements, unless the `if` statement is the first statement inside a block `{`.

### How to Build and Use

Custom clang-tidy checks are typically compiled into a dynamic plugin or built directly inside the LLVM `clang-tools-extra` source tree.

#### Building as a Clang-Tidy Plugin

You can build this as a standalone shared library (`.so`) if you have LLVM/Clang development packages installed.

1. Create a `CMakeLists.txt` here using LLVM's CMake macros (`add_llvm_library(... MODULE)`).
2. Compile it to produce `EmptyLineBeforeIfPlugin.so`.
3. Load the plugin and run it on your code:

```bash
clang-tidy -load=./EmptyLineBeforeIfPlugin.so -checks="-*,readability-empty-line-before-if" your_file.cpp
```

*Note: For complex logic (e.g., properly ignoring block comments `/* ... */` and line comments `// ...` before the `if`), you may need to extend the character stream parser or integrate `Lexer::getRawToken` to navigate token boundaries more accurately.*