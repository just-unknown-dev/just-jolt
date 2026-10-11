/// Jolt Physics (v5.6.0) for Dart.
///
/// [Jolt] says whether the native library is in this build and starts its
/// job system. The bindings themselves — joltc's C API (`JPH_*`) for shapes
/// and the body interface, and just_jolt's C helpers (`jj_*`: worlds,
/// batched reads, buffered events, queries, characters, joints) — are in
/// `package:just_jolt/ffi.dart`, for native platforms.
///
/// The library is compiled from source only when the app asks for it in its
/// own pubspec:
///
/// ```yaml
/// hooks:
///   user_defines:
///     just_jolt:
///       build: true
/// ```
///
/// On the web [Jolt.start] says Jolt cannot run.
library;

export 'src/jolt_loader_stub.dart' if (dart.library.ffi) 'src/jolt_loader.dart';
