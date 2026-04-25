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
clang-tidy -checks='-*,modernize-*,performance-*,readability-*' lib/**/*.cpp
```

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for code style guidelines.

## AI Agents

If you are an AI agent working on this project, please read [AGENTS.md](AGENTS.md) for environment setup and working rules.
