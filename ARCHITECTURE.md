# just_jolt architecture

```
lib/just_jolt.dart        Jolt (conditional: src/jolt_loader_stub.dart on the web)
lib/ffi.dart              native only: Jolt and the bindings
lib/src/jolt_loader.dart  Jolt: jj_init once, availability, hot-restart clean-up
lib/src/native/bindings.g.dart   ffigen (ffi-native): jj_* + joltc shapes/body interface
hook/build.dart, hook/unity.dart two-stage native build, only with `build: true`
src/jj/jj.h, jj_capi.cpp  our C layer (one TU with joltc.cpp)
src/jolt/Jolt, src/joltc  vendored Jolt v5.6.0 and joltc 886e088
```

just_jolt is plain Dart and depends on no other just_* package: it makes Jolt usable from Dart and knows nothing of any engine. just_physics_engine's `JoltPhysicsEngine` is one user.

## Native build

Nothing is built unless the app's own pubspec sets `hooks: user_defines: just_jolt: build: true` (only the root package's user-defines reach a hook). An app that has just_jolt only through another package — a 2-D game on a physics engine that can also do 3-D — neither compiles nor ships Jolt, and `Jolt.start` says what to add.

Stage 1 makes the static `jolt_core` from 6 generated unity files; stage 2 makes the bundled `just_jolt` library from `jj_capi.cpp`, which `#include`s `joltc.cpp` and `joltc_assert.cpp` so our layer can use Jolt's C++ API and joltc's internals (its `JPH_PhysicsSystem` wrapper) directly. Both stages get one defines map; `JPH_SHARED_LIBRARY_BUILD` (stage 2) exports joltc's `JPH_*` functions along with our `jj_*` ones. On Android only arm64-v8a and x86_64 build: finalizer tokens carry 64-bit handles in a pointer.

## Who calls what

| From Dart | Through |
|---|---|
| Shapes (box … height field, offsets) | joltc `JPH_*ShapeSettings_*` / `JPH_RotatedTranslatedShape_Create` |
| Velocities, forces, impulses, friction, gravity factor, motion quality, teleports, kinematic moves, wake | joltc `JPH_BodyInterface_*` on the world's body interface (`jj_world_joltc`) |
| Worlds, bodies, layers, batch reads, events, queries, joints, characters, debug lines | `jj_*` |

Calls through joltc's body interface take a raw `BodyID`. A caller should only make them for handles carrying the world's own tag (`jj_world_handle_tag`), and never move a static body kinematically (Jolt would dereference its missing motion properties). Jolt itself checks the `BodyID`'s sequence number, so a destroyed body's ID is passed over.

## Axes and material mixing

The layer assumes no axis convention: a world takes its gravity and a character its up (zero means Jolt's +Y). `JJ_WorldSettings.restitutionCombine` picks how a contact mixes its bodies' restitution — `JJ_COMBINE_DEFAULT` (Jolt's rule, the bouncier), `_MIN`, `_MAX` or `_AVERAGE`. Friction mixes by Jolt's rule, the geometric mean.

## Layers

Object layer = kind × 16 + layer, 64 in all; kinds are static, moving (dynamic and kinematic), sensor and character (a `CharacterVirtual`'s inner body), each its own broad-phase layer. Two object layers collide when the grid says their layers do (both rows) and their kinds may touch:

| | static | moving | sensor | character |
|---|---|---|---|---|
| static | – | ✓ | ✓ | – |
| moving | ✓ | ✓ | ✓ | ✓ |
| sensor | ✓ | ✓ | – | ✓ |
| character | – | ✓ | ✓ | – |

Built once per world into Jolt's `ObjectLayerPairFilterTable`, `BroadPhaseLayerInterfaceTable` and `ObjectVsBroadPhaseLayerFilterTable`. Queries filter by a layer mask and two flags (sensors, characters). A character's own sweeps see static and moving bodies on its grid row; character-versus-character collision (`CharacterVsCharacterCollisionSimple`) is limited to characters whose layers collide.

## Threads, locks and teardown

- One `JobSystemThreadPool` for the process, made by `jj_init` and never destroyed, so no step can outlive it. Each world has its own `TempAllocatorImplWithMallocFallback`.
- Every entry point takes one recursive lock: a `NativeFinalizer` may run on another thread, and a destroy must never overlap a step. Jolt's listeners run on worker threads during a step and only append to the world's event buffer under its own mutex; nothing calls Dart.
- A world is torn down listeners first, then characters (their inner bodies need the system), joints (they point at bodies), bodies, the system, and what it used.
- Handles: world = generation(32) | slot+1; object = generation(14) | type(2) | slot+1(16) | low(32), where low is the `BodyID` or a 12-bit sequence and 20-bit index. Every function resolves and checks a handle before use; dead ones do nothing.
- `Jolt.start`, in a debug build, calls `jj_destroy_all` the first time an isolate starts Jolt: a hot restart drops the Dart objects but not the native worlds. Use Jolt from one isolate per process (`dart test` runs this package's files one at a time for that reason, `dart_test.yaml`).
- **Windows: the Dart profiler and the stack.** In debug and profile builds Dart's sampling profiler walks the isolate thread's native stack by following RBP, checking only that each frame pointer lies inside the thread's stack. MSVC's x64 code keeps no frame pointers, so while Jolt runs RBP can hold any value; if it points at stack that was never used (reserved, not yet committed), the profiler's read crashes the process — measured at about one `flutter test` run in 25, always the first steps of the first world, when Jolt first takes the stack deeper than anything before it. Every entry point that runs deep Jolt code therefore commits the calling thread's stack once (`commitStack`, all but the last 128 KB), so there is no uncommitted stack left to read. Linux, Android and Apple stacks are readable throughout and need nothing.

## Events

Contact begin/end are counted per body pair (a compound touches with many sub-shapes). Jolt drops a pair's contact when a body falls asleep and finds it again when it wakes; the layer keeps such a pair "dormant" with no end event, resumes it silently, and only reports an end if the bodies are awake for a step without touching, or one of them is destroyed. Stay events (one per pair per step) and activation events are opt-in per world.

## Characters

`jj_character_step` sets the velocity Dart computed and runs `CharacterVirtual::ExtendedUpdate` with the world's gravity, the step-down (stick to floor) and step-up (stairs) vectors. After each world step every character's ground velocity is refreshed, so the next velocity Dart computes already includes a moving platform's motion. The capsule is shifted along "up" so the character's position is its feet.

## Tests

`dart test` (the VM): `test/native_test.dart` (raw bindings: create/step/destroy, handle kinds, teardown in any order, 200 worlds, repeated hot restarts), `test/loader_test.dart` (`Jolt`), `test/restitution_test.dart` (each combine rule on a bouncy ball and a dead floor) and `test/build_switch_test.dart` (the hook builds nothing without `build: true`). `test/web_test.dart` runs in a browser (`dart test -p chrome`, with or without `--compiler dart2wasm`) and checks that `package:just_jolt/just_jolt.dart` compiles there and `Jolt` says it cannot run. Bodies, events, queries, joints and characters through a whole physics interface are tested where that interface lives, in just_physics_engine (`test/physics_3d/jolt_*_test.dart`).
