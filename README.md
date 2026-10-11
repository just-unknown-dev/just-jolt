# Just Jolt

[Jolt Physics](https://github.com/jrouwe/JoltPhysics) v5.6.0 for Dart. Jolt is reached through the [joltc](https://github.com/amerkoleci/joltc) C API and a small C layer of our own (`src/jj/`) that gives Dart what joltc cannot: batched reads, events buffered off Jolt's worker threads, queries into caller buffers, characters and joints. Everything is compiled from source by the package's build hook. Plain Dart: it works in Flutter apps and Dart programs alike, and depends on no engine.

```yaml
# The app's pubspec.yaml
dependencies:
  just_jolt: ^0.1.0

hooks:
  user_defines:
    just_jolt:
      build: true # without this nothing is compiled or shipped
```

```dart
import 'dart:ffi';

import 'package:ffi/ffi.dart';
import 'package:just_jolt/ffi.dart';

void main() {
  final reason = Jolt.start();
  if (reason != null) throw UnsupportedError(reason);
  using((Arena arena) {
    final settings = arena<JJ_WorldSettings>();
    settings.ref.gravity[1] = -9.81;
    for (var i = 0; i < JJ_LAYER_COUNT; i++) {
      settings.ref.collides[i] = 0xFFFF;
    }
    final world = jj_world_create(settings);
    // … jj_body_create, jj_world_step, jj_get_active_transforms …
    jj_world_destroy(world);
  });
}
```

- `package:just_jolt/just_jolt.dart` — `Jolt`: whether the library is in this build, its version, and its one job system. Safe to import on the web, where it says Jolt cannot run.
- `package:just_jolt/ffi.dart` — native platforms only: `Jolt` and the generated bindings (`jj_*` and joltc's `JPH_*` functions for shapes and the body interface).

The whole program, a ball dropped on a floor, is in [`example/example.dart`](example/example.dart). For a game engine's 3D physics on top of it, see just_physics_engine's `JoltPhysicsEngine`.

## Why the build switch

Only the app's own pubspec can turn a hook's user-defines on, so an app that gets just_jolt through another package (a 2-D game on a physics engine that can also do 3-D) builds nothing: no compile time, no 1.5–2.7 MB library, no licences to ship. `Jolt.start` says what to add when the library is missing.

## Platforms

| Platform | Status |
|---|---|
| Windows x64 | Built and tested here (MSVC). |
| Android arm64-v8a, x86_64 | Built by the hook (NDK r26 or newer); checked on a device by hand. 16 KB page-aligned, with the C++ runtime linked in (no `libc++_shared.so` to ship). 32-bit ABIs build nothing, and `Jolt.start` says so. |
| Linux, macOS, iOS | Should build (clang); untested. |
| Web (JavaScript and WebAssembly) | `package:just_jolt/just_jolt.dart` compiles, and `Jolt.start` says Jolt cannot run (no `dart:ffi`). Don't import `package:just_jolt/ffi.dart` in web code. |

On Windows, MSVC and Dart's directory listing cannot open a path longer than 259 characters. The hook builds in the app's `.dart_tool` folder, so keep the app's folder path under about 175 characters; a build that would pass the limit stops and names the path.

## How it builds

There is no CMake step. `hook/build.dart` compiles everything when an app builds with `build: true`:

1. **`jolt_core`** (static, never bundled): Jolt's 130 unconditional sources, grouped into 6 unity files generated in the build directory from the source list in `src/jolt/Jolt/Jolt.cmake` (`hook/unity.dart`). Optional parts (ObjectStream, GPU compute backends, shader wrappers) are left out.
2. **`just_jolt`** (the bundled library, asset id `package:just_jolt/just_jolt`): `src/jj/jj_capi.cpp`, which compiles joltc and our `jj_*` layer in one translation unit.

Both stages use the same `JPH_*` defines; Jolt checks them when it initialises and aborts on a mismatch.

| | |
|---|---|
| Defines | `JPH_OBJECT_LAYER_BITS=32`; on x64 `JPH_USE_SSE4_1`, `JPH_USE_SSE4_2` (no AVX); stage 2 `JPH_SHARED_LIBRARY_BUILD` |
| MSVC | `/GR- /fp:fast /bigobj /Zc:inline` |
| clang | `-fno-rtti -fno-exceptions -fvisibility=hidden -pthread`; x64 adds `-msse4.2 -mpopcnt` |
| Android / Linux | link `libm`; Android links `c++_static` |

The first Windows build takes about 40 s and makes a 1.5 MB DLL; the Android library is about 2.7 MB per ABI.

### Dev flavour

Jolt's debug renderer (`jj_debug_lines`) and its asserts (printed on stderr, never a breakpoint) are compiled in when the app asks:

```yaml
hooks:
  user_defines:
    just_jolt:
      build: true
      dev: true
```

## Our C layer (`src/jj/jj.h`)

joltc covers most of Jolt; the layer adds what joltc lacks or what cannot cross into Dart:

- **Worlds** whose collision layers come from a 16 × 16 grid, through Jolt's table filters, with broad-phase layers for static, moving, sensor and character bodies. Each world owns its temporary allocator; one job system serves the whole process. A world picks how contacts mix restitution (`restitutionCombine`: Jolt's rule, min, max or average).
- **Events.** Jolt calls its listeners on worker threads in the middle of a step, so they cannot call into Dart. Contact begin/stay/end, sensor begin/end and body sleep/wake are buffered natively and drained after the step. A body falling asleep keeps its contacts (Jolt drops and refinds them; the layer hides that).
- **Batch reads**: the moving bodies' poses and velocities in one call, and those that fell asleep since the last read.
- **Queries** into caller-owned buffers: raycast, shape cast, overlap; closest or all hits; a layer mask and one ignored body.
- **Characters** (`CharacterVirtual`): `jj_character_step` sets the velocity and runs `ExtendedUpdate` — walking stairs, sticking to the floor — and returns the ground state, normal and velocity. With an inner body, sensors detect the character and rays hit it.
- **Joints**: fixed, point, hinge, slider, distance and six-DOF, with limits and motors. A hinge's angle and a slider's position are body A's, measured from body B.
- **Teardown in any order.** Handles are int64s packing a world slot and generation with a `BodyID` or an index and sequence, checked on every call: a handle from a destroyed world never reaches a new one. Destroying a body destroys its joints. `jj_destroy_all` frees everything on a hot restart.

No axis convention is assumed: a world takes its gravity and a character its up (zero means Jolt's +Y).

## Updating Jolt or joltc

1. Replace `src/jolt/Jolt/` (only that folder, plus `src/jolt/LICENSE`) and the three files in `src/joltc/`.
2. `dart run ffigen --config ffigen.yaml` regenerates `lib/src/native/bindings.g.dart`.
3. Run the tests (`dart test`). The hook regenerates the unity files from the new `Jolt.cmake`; a source that clashes inside a unity file goes in `isolatedSources` in `hook/unity.dart`.

## Licences

just_jolt's own code is under the BSD 3-Clause licence. It builds and bundles **Jolt Physics** v5.6.0 (© 2021 Jorrit Rouwe, MIT) and **joltc** (commit `886e088`, © Amer Koleci and Contributors, MIT). All three texts are in `LICENSE`, in the form Flutter reads, so a Flutter app's licence page lists them without any code.
