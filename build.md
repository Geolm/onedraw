# Build & Test

- Config: `cmake -B build -S . -DCMAKE_BUILD_TYPE=Release`
- Build: `cmake --build build`
- Clean: `rm -rf build/`
- Test: `./build/test --autoexit`
- Check that `screenshot.tga` is written
- Every bug fix requires a regression test.
- Verify tests actually run before claiming success.