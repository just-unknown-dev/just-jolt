/// just_jolt's native API, for native platforms only (it imports
/// `dart:ffi`):
///
/// - [Jolt], to start Jolt before anything else;
/// - the generated bindings: just_jolt's C helpers (`jj_*`, see
///   `src/jj/jj.h`) and the part of joltc that Dart uses directly (shapes
///   and the body interface, `JPH_*`).
library;

export 'src/jolt_loader.dart';
export 'src/native/bindings.g.dart';
