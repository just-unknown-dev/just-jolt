import 'native/bindings.g.dart';

/// Jolt Physics in this process: whether its native library is in the
/// build, and its one job system.
///
/// Call [start] before making a world (`jj_world_create`). The first call
/// decides the job system's size; later calls return the first answer.
///
/// In a debug build the first [start] of an isolate also destroys every
/// world a previous isolate left behind: after a hot restart the old Dart
/// objects are gone but the native worlds are not. Use Jolt from one
/// isolate per process.
abstract final class Jolt {
  static bool _started = false;
  static String? _reason;

  /// Starts Jolt with a job system of [workerThreads] threads (below 0:
  /// cores minus one). Null when Jolt is ready, otherwise why it is not —
  /// most often that the app did not ask for the library to be built.
  static String? start({int workerThreads = -1}) {
    if (_started) return _reason;
    _started = true;
    return _reason = _start(workerThreads);
  }

  /// Whether [start] succeeded (starting Jolt if it has not been).
  static bool get isAvailable => start() == null;

  /// Jolt's version, `major.minor.patch`; null when it is not available.
  static String? get version {
    if (!isAvailable) return null;
    final v = jj_version();
    return '${v >> 16}.${(v >> 8) & 0xFF}.${v & 0xFF}';
  }

  /// Whether the library has Jolt's debug renderer (the dev flavour).
  static bool get hasDebugRenderer => isAvailable && jj_has_debug_renderer();

  /// How many native worlds are alive in this process (for leak checks).
  static int get liveWorlds => isAvailable ? jj_live_worlds() : 0;

  /// Destroys every native world in this process, whoever made it.
  static void destroyAll() {
    if (isAvailable) jj_destroy_all();
  }

  static String? _start(int workerThreads) {
    try {
      if (!jj_init(workerThreads)) return 'Jolt did not initialise.';
    } on Object catch (error) {
      return 'the just_jolt native library is not in this build. Add '
          '`build: true` under `hooks: user_defines: just_jolt:` in the '
          "app's pubspec.yaml (on Android it is built for arm64-v8a and "
          'x86_64 only). ($error)';
    }
    // Only a debug build has hot restarts.
    if (!const bool.fromEnvironment('dart.vm.product') &&
        !const bool.fromEnvironment('dart.vm.profile')) {
      jj_destroy_all();
    }
    return null;
  }
}
