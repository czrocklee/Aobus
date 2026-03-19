# RockStudio

C++23 music library application with LMDB storage.

## Building

```bash
# Debug build (default)
./build.sh

# Release build
./build.sh release

# Clean build
./build.sh debug clean
```

## Running Tests

```bash
/tmp/build/rs_test
```

## Static Analysis

```bash
clang-tidy -checks='-*,modernize-*,performance-*,readability-*' src/**/*.cpp
```

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for code style guidelines.
