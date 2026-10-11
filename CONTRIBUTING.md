# Contributing to Just Jolt

Thank you for your interest in contributing to **Just Jolt**. Bug fixes, features, tests and documentation are all welcome.

---

## Table of Contents

- [Code of Conduct](#code-of-conduct)
- [Reporting Bugs and Suggesting Features](#reporting-bugs-and-suggesting-features)
- [Development Setup](#development-setup)
- [Who Uses the Package](#who-uses-the-package)
- [How the Package Fits Together](#how-the-package-fits-together)
- [Changing the C Layer](#changing-the-c-layer)
- [Updating Jolt or joltc](#updating-jolt-or-joltc)
- [Running the Tests](#running-the-tests)
- [Coding Guidelines](#coding-guidelines)
- [Submitting a Pull Request](#submitting-a-pull-request)
- [Commit Message Convention](#commit-message-convention)
- [License](#license)

---

## Code of Conduct

This project follows our [Code of Conduct](CODE_OF_CONDUCT.md). By taking part you agree to its terms. Please report unacceptable behaviour to the maintainers.

---

## Reporting Bugs and Suggesting Features

Before opening an issue, search the [existing issues](https://github.com/just-unknown-dev/just-jolt/issues) and check that the problem happens on the latest version.

A bug report should have:

- a clear title;
- steps to reproduce, and what you expected instead of what happened;
- the output of `dart --version` (or `flutter --version` in a Flutter app), the platform and architecture (Windows x64, Android arm64-v8a, …), and the C++ toolchain: the Visual Studio, clang or Xcode version, or the Android NDK version;
- whether the app set `build: true` alone or `dev: true` as well, and what `Jolt.start()` returned;
- for a build failure, the hook's whole output;
- for a crash or a wrong result, the smallest Dart program that shows it: the `JJ_WorldSettings`, the shapes and bodies, and the `jj_*` calls up to the failure;
- for a crash on Windows, whether it happens in a release build too.

Bugs often belong somewhere else:

- If you only see the bug through `JoltPhysicsEngine` or a game engine's 3D physics, and cannot show it with `jj_*` calls alone, it belongs in [just_physics_engine](https://github.com/just-unknown-dev/just-physics-engine/issues).
- If Jolt's own C++ API behaves the same way, it belongs [upstream in Jolt Physics](https://github.com/jrouwe/JoltPhysics/issues). A bug in joltc's `JPH_*` functions belongs in [joltc](https://github.com/amerkoleci/joltc/issues).

For a feature, open an issue labelled **`enhancement`** that describes the problem it solves and how it should behave. The package's scope is making Jolt usable from Dart: building it, the bindings, and the `jj_*` layer for what joltc cannot give Dart. It knows nothing of any engine: components, scenes and axis conventions belong in the packages that use it. A change to `src/jj/jj.h` changes the API that just_physics_engine calls, so open the issue first and agree the approach before you write it.

---

## Development Setup

| Tool | Minimum version |
|------|-----------------|
| Dart | 3.11.0 |
| C++17 toolchain | Windows: Visual Studio with the "Desktop development with C++" workload. Linux: clang. macOS and iOS: Xcode. Android: NDK r26 or newer |
| LLVM | Only to regenerate the bindings (ffigen needs libclang) |
| Chrome | Only for the web test |

The package is plain Dart: you do not need Flutter.

```bash
git clone https://github.com/just-unknown-dev/just-jolt.git just_jolt
cd just_jolt
dart pub get
dart test
```

The package's own `pubspec.yaml` turns the native build on (`hooks: user_defines: just_jolt: build: true`), so the first `dart test` compiles Jolt from source. That takes about 40 seconds on Windows. On Windows, keep the clone's path short: MSVC cannot open paths longer than 259 characters, and the build's deepest files sit about 80 characters below the repository folder.

To try a change in an app, add the package as a path dependency (`path: ../just_jolt`) and set `build: true` in the app's own pubspec. Only the root package's user-defines reach the hook, so without it nothing is built.

If you're working from the Just engine monorepo, run every command from `packages/just_jolt`.

---

## Who Uses the Package

just_jolt depends on no other just_* package, so there is nothing to override here.

Its user is just_physics_engine. `JoltPhysicsEngine` (`lib/src/physics_3d/jolt/`) calls the bindings, and its tests (`test/physics_3d/jolt_*_test.dart`) cover bodies, events, queries, joints and characters through a whole physics interface. just_game_engine and just_third_person_kit set `build: true` in their own pubspecs so their tests can run 3D physics.

To check a change against just_physics_engine, check it out next to this repository and point it at your copy with a `pubspec_overrides.yaml` in its root:

```yaml
# pubspec_overrides.yaml: local only, never commit it.
dependency_overrides:
  just_jolt:
    path: ../just_jolt
```

Then run `flutter pub get` and `flutter test test/physics_3d` there. If you change `jj.h` or the bindings, say in the pull request what just_physics_engine will need.

---

## How the Package Fits Together

| Path | Contents |
|------|----------|
| `lib/just_jolt.dart` | `Jolt`, safe to import on every platform. On the web it exports `lib/src/jolt_loader_stub.dart`, which says Jolt cannot run |
| `lib/ffi.dart` | Native platforms only: `Jolt` and the generated bindings |
| `lib/src/jolt_loader.dart` | `Jolt`: starts Jolt once, says whether the library is in the build, frees old worlds after a hot restart |
| `lib/src/native/bindings.g.dart` | The bindings, generated by ffigen from `ffigen.yaml`. Never edit it by hand |
| `hook/build.dart` | The native build: Jolt's sources, read from `Jolt.cmake` and grouped into unity files, make the static `jolt_core`, then the bundled `just_jolt` library. pub.dev accepts no other file in `hook/`, so helpers stay in this file |
| `src/jj/` | Our C layer: `jj.h` and `jj_capi.cpp`, compiled in one translation unit with joltc |
| `src/jolt/` | Jolt Physics v5.6.0, vendored |
| `src/joltc/` | joltc (commit `886e088`), vendored |
| `test/` | The tests |

[ARCHITECTURE.md](ARCHITECTURE.md) explains the layers, handles, threads and events in more depth.

A few rules hold the package together. Keep them:

- **Plain Dart, no engine.** No Flutter dependency and no other just_* package. The C layer assumes no axis: a world takes its gravity and a character its up. Units are metres, seconds and radians.
- **Nothing is built unless the app asks.** The hook builds nothing unless the root pubspec sets `build: true`, and `dev: true` alone builds nothing either. A 2-D game that gets just_jolt through just_physics_engine must neither compile nor ship Jolt. `test/build_switch_test.dart` checks it.
- **Both build stages share one defines map.** Jolt compares the `JPH_*` defines at `JPH_Init` and aborts on a mismatch. Add a define to `defines` in `hook/build.dart`, never to one stage only.
- **Nothing calls into Dart from Jolt's threads.** Jolt calls its listeners on worker threads in the middle of a step. They only append to the world's event buffer, and Dart drains it after the step.
- **Every entry point takes the lock and checks its handles.** Each `jj_*` function takes the one recursive lock (`gLock`): a `NativeFinalizer` may run on another thread, and a destroy must never overlap a step. It resolves every handle before using it and does nothing with a dead one. Handles are int64 and never reused, so a handle from a destroyed world never reaches a new one.
- **Deep Jolt calls commit the stack.** An entry point that runs Jolt code deeper than a lookup calls `commitStack()` right after taking the lock, as the existing ones do. On Windows, Dart's sampling profiler can otherwise crash a debug or profile build. `ARCHITECTURE.md` has the details.
- **Teardown works in any order.** Destroying a body destroys its joints, a world finalized first takes its bodies with it, and `jj_destroy_all` frees everything after a hot restart. When you add a kind of object, extend the "teardown in any order" group in `test/native_test.dart`.
- **One job system, one isolate.** `jj_init` makes one `JobSystemThreadPool` for the process and never destroys it. Use Jolt from one isolate per process.
- **The vendored sources stay as upstream ships them.** Updating Jolt or joltc replaces their folders, so a local patch would be lost. Put the change in `src/jj/`, or send it upstream.
- **Android builds only arm64-v8a and x86_64.** Finalizer tokens carry 64-bit handles in a pointer. On other ABIs the hook bundles nothing, and `Jolt.start` says so.

---

## Changing the C Layer

1. Declare the function in `src/jj/jj.h` with `JJ_API` and a comment, and write it in `src/jj/jj_capi.cpp`, following the rules above. Name functions `jj_*` and structs and macros `JJ_*`: `ffigen.yaml` only picks up those names.
2. If Dart should make it a leaf call, add it under `leaf:` in `ffigen.yaml`. A leaf call must be short and must never wait on Jolt's threads: a world step, a character step, and making or destroying worlds and characters are ordinary calls. A function a `NativeFinalizer` calls goes under `symbol-address:`.
3. Regenerate the bindings:

   ```bash
   dart run ffigen --config ffigen.yaml
   ```

   ffigen needs LLVM. `ffigen.yaml` looks in LLVM's default Windows folders. Elsewhere, install LLVM where ffigen looks for it, or add its folder to `llvm-path`.
4. Run `dart test`. The hook rebuilds the library.

---

## Updating Jolt or joltc

1. Replace `src/jolt/Jolt/` (only that folder) and `src/jolt/LICENSE`, or the files in `src/joltc/`.
2. Regenerate the bindings (see above).
3. Run `dart test`. The hook makes the unity files again from the new `Jolt.cmake`. A source that clashes with another inside a unity file goes in `isolatedSources` in `hook/build.dart`.
4. Update the version everywhere it is written: the description in `pubspec.yaml`, `README.md`, `ARCHITECTURE.md`, the version checks in `test/loader_test.dart` and `test/native_test.dart`, `CHANGELOG.md`, and the Jolt or joltc text in `LICENSE`. Keep `LICENSE` in the form Flutter's licence page reads.
5. Run just_physics_engine's 3D tests against the update.

---

## Running the Tests

Run every command from the repository root.

```bash
# The whole suite.
dart test

# One file.
dart test test/native_test.dart

# The web test, compiled to JavaScript, then to WebAssembly.
dart test -p chrome
dart test -p chrome --compiler dart2wasm
```

`dart_test.yaml` sets `concurrency: 1`. Jolt's worlds and its job system belong to the process, and `dart test` runs test files as isolates of one process, so the files run one at a time.

| File | What it checks |
|------|----------------|
| `test/native_test.dart` | The raw bindings: create, step and destroy; handle kinds; teardown in any order; 200 worlds; repeated hot restarts |
| `test/loader_test.dart` | `Jolt`: starting, the version, counting and destroying live worlds |
| `test/restitution_test.dart` | Each `restitutionCombine` rule, on a bouncy ball and a dead floor |
| `test/build_switch_test.dart` | The hook builds nothing without `build: true` |
| `test/web_test.dart` | In a browser: `package:just_jolt/just_jolt.dart` compiles, and `Jolt` says it cannot run |

The other files are marked `@TestOn('vm')`, so `-p chrome` runs only the web test.

Bodies, events, queries, joints and characters through a whole physics interface are tested in just_physics_engine. Run its `test/physics_3d` tests too when you change the C layer.

**The dev flavour.** To build with Jolt's debug renderer and asserts, add `dev: true` beside `build: true` in this package's `pubspec.yaml`, and take it out again before you commit. The debug lines test in `test/native_test.dart` checks the library either way.

**Writing tests.** Use the bindings with an `Arena` (`using`), as the existing tests do. Destroy every world a test makes; `Jolt.liveWorlds` counts the ones left.

---

## Coding Guidelines

- **Formatting.** Dart code is formatted with `dart format`. This must report no changes:

  ```bash
  dart format --output=none --set-exit-if-changed lib test hook example
  ```

  In `src/jj/`, match the style of the code around you.
- **Analysis.** `dart analyze` must report no issues at all: no errors, warnings or infos. The rules are in `analysis_options.yaml`: `package:lints/recommended.yaml`, with `build/` and `src/` excluded.
- **C++.** C++17, built without RTTI and without exceptions (`/GR-`, `-fno-rtti -fno-exceptions`), so no `dynamic_cast`, `typeid` or `throw`.
- **Web.** `lib/just_jolt.dart` must stay importable on the web. It reaches `dart:ffi` only through its conditional export. Anything else that needs `dart:ffi` goes behind `lib/ffi.dart`.
- **Documentation.** Give every public Dart API a `///` doc comment, and every function and struct in `jj.h` a comment: ffigen copies them into the bindings. Describe user-visible changes in [CHANGELOG.md](CHANGELOG.md). Update [README.md](README.md) and [ARCHITECTURE.md](ARCHITECTURE.md) when the build, the C layer or the rules change.
- **Tests.** Add or update tests for any behaviour you change. A bug fix comes with a test that fails without it.
- **Dependencies.** Do not add a dependency without discussing it in an issue first. The package stays free of Flutter and of other just_* packages.

---

## Submitting a Pull Request

1. Create a topic branch from `main`:
   ```bash
   git checkout -b fix/joint-teardown-order
   ```
2. Make your change, following the [Coding Guidelines](#coding-guidelines).
3. Check that everything passes:
   ```bash
   dart format --output=none --set-exit-if-changed lib test hook example
   dart analyze
   dart test
   dart test -p chrome
   dart test -p chrome --compiler dart2wasm
   ```
   For a change to the C layer, also run just_physics_engine's `flutter test test/physics_3d` against your copy.
4. Commit with a [conventional commit message](#commit-message-convention).
5. Push to your fork and open a pull request against `main`. Say what changed and why, which platforms you built and tested on, and anything just_physics_engine will need.
6. Answer review feedback.

Small pull requests are easier to review.

---

## Commit Message Convention

We use [Conventional Commits](https://www.conventionalcommits.org/):

```
<type>(<scope>): <short summary>
```

| Type | When to use |
|------|-------------|
| `feat` | New feature |
| `fix` | Bug fix |
| `docs` | Documentation only |
| `refactor` | Code change that neither fixes a bug nor adds a feature |
| `perf` | Performance improvement |
| `test` | Adding or updating tests |
| `chore` | Build process, tooling, dependencies |

Use the area as the scope: `hook`, `jj`, `bindings`, `loader` or `vendor` (an update of Jolt or joltc). Mark a change that breaks the `jj_*` API or the Dart API with `!` and a `BREAKING CHANGE:` footer.

**Examples:**
```
fix(jj): keep a sleeping pair's contact until the bodies wake apart
feat(jj): add a cone twist joint
chore(vendor): update Jolt Physics to 5.7.0
```

---

## License

By contributing to Just Jolt you agree that your contributions will be licensed under the [BSD-3-Clause License](LICENSE). The vendored Jolt Physics and joltc keep their own MIT licences; their texts are in the same file.
