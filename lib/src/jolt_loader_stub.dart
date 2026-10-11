/// Jolt Physics where there is no `dart:ffi` (the web): never available.
abstract final class Jolt {
  /// Why Jolt cannot run here.
  static String? start({int workerThreads = -1}) =>
      'Jolt needs dart:ffi, which the web does not have.';

  /// Always false here.
  static bool get isAvailable => false;

  /// Always null here.
  static String? get version => null;

  /// Always false here.
  static bool get hasDebugRenderer => false;

  /// Always 0 here.
  static int get liveWorlds => 0;

  /// Does nothing here.
  static void destroyAll() {}
}
