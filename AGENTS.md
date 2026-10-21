# AGENTS.md

Don't modify this file.

## Code & API

- C11 core; public headers C++ compatible. No unapproved API changes.
- C++ files: C-style C++ only. No exceptions, RTTI, STL, iostream, lambdas, virtual methods, inheritance, operator overloading, or new/delete (use the provided allocator or local variables). Avoid `auto`. All C rules apply.
- Use `bool`; prefer `uint32_t`/`uint64_t` unless negatives are needed.
- Style: `//` comments, Allman braces, 4 spaces, no tabs. Preserve existing conventions; don't reformat unrelated code.
- Avoid needless abstractions/macros. Factor duplicated blocks >50 lines.
- Line length: 160 chars. Separators: 127 chars (`// ----`).
- Prefer designated initializers for struct initialization. Use compound literals with designated initializers when appropriate and when they improve readability.


## Code Quality

- Prefer the simplest correct implementation: minimize LoC, branches, helpers, and duplication without hurting readability, correctness, or maintainability.
- Keep concise code clear: no cryptic names, excessive compression, clever tricks, or obfuscation.
- Before adding code, reuse existing logic, remove redundancy, or simplify the design. Prefer deleting code over adding abstractions.

## Memory

- Libraries: no `malloc`/`free`; prefer caller-provided buffers. If a library needs more than the caller-provided buffer, return an error or a required-size value; don't allocate behind the caller's back.
- C++: allocate only via the provided allocator or local variables.
- Tools: `malloc`/`free` allowed outside critical hot loops.
- Up to 256 KiB: static buffer. Over 256 KiB: project memory interface.
- Make ownership explicit. Don't retain caller pointers unless required. Avoid hidden allocations and large temporary copies.

## State & Dependencies

- No mutable global state; use explicit contexts/parameters.
- Single-threaded: no mutexes, atomics, or lock-free structures.
- No unapproved third-party dependencies. Preserve platform/build requirements.
- Use RNG SplitMix32; never `rand()`.

## Performance

- Favor compact layouts, cache locality, and linear buffer access.
- Avoid redundant passes over large data.
- Optimize only based on measured/profiled improvements; no speculative complexity.
- Never sacrifice correctness or maintainability for unmeasured optimization.

## Build & Test

- Config: `cmake -B build -S . -DCMAKE_BUILD_TYPE=Release`
- Build: `cmake --build build`
- Clean: `rm -rf build/`
- Test: `./build/test --autoexit`
- Every bug fix requires a regression test. Adding new tests is allowed.
- Never modify/weaken tests to make them pass.
- Verify tests actually run before claiming success.
- Test behavior/results, not implementation details; don't enforce a specific implementation.
- No harness tests (tests that only exercise test scaffolding rather than product code).

## Git

- Don't create a branch without permission
- Don't submit code without permission

## Completion

- Before reporting completion, verify: implementation finished, build succeeds, tests pass, rules followed, and no unrelated code changed.
- In case of failure (unit test, not reaching goal, etc...), write the experiment description in failed_experiments.md