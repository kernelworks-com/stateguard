# Contributing

## Build and test

[docs/BUILDING.md](docs/BUILDING.md) has the full instructions. From the
repository root:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS="-Wall -Wextra -Wpedantic -Werror"
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure --no-tests=error
python3 scripts/check_install.py --workdir build-install-check
```

CI runs three configurations, and a change must pass all of them:

| Runner | Compiler | Configuration |
|---|---|---|
| Ubuntu 24.04 | GCC 13 | Release, warnings as errors, install checks |
| Ubuntu 24.04 | Clang 18 | Debug, address and undefined-behavior sanitizers |
| macOS 14 | Apple Clang | Release, warnings as errors, install checks |

For the sanitizer job use `clang++-18`, `Debug`, and add
`-fsanitize=address,undefined -fno-omit-frame-pointer` to the flags.
Warnings are errors in every configuration.

## Rules

- Test fixtures and sample traces must be synthetic. Never commit production
  captures or traces derived from them.
- Keep pull requests small and focused on one change; add or update tests with
  behavior changes.
- Public documentation states only what was actually run.
- Use a [Conventional Commits](https://www.conventionalcommits.org/) subject.

## Pull request description

Use these sections:

```markdown
## Summary
What changed and why.

## Testing
Commands you ran and their results.

## Limits
What was not tested or is not covered.
```
