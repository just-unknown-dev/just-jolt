import 'dart:io';

import 'package:code_assets/code_assets.dart';
import 'package:hooks/hooks.dart';
import 'package:native_toolchain_c/native_toolchain_c.dart';

import 'unity.dart';

/// Builds Jolt Physics and its C API in two stages.
///
/// 1. `jolt_core`, a static library, from unity files generated into the
///    build directory (see `unity.dart`). It is linked into stage 2 and never
///    bundled.
/// 2. `just_jolt`, the bundled dynamic library, from `src/jj/jj_capi.cpp`,
///    which compiles joltc and our `jj_*` layer in one translation unit.
///
/// Both stages get the same JPH_* defines: Jolt checks them at `JPH_Init` and
/// calls `std::abort()` when the library and its caller disagree.
///
/// Nothing is built unless the app asks for it in its own pubspec, so an app
/// that has just_jolt only as a dependency of a dependency (a 2-D game on a
/// physics engine that can also do 3-D) neither compiles nor ships Jolt:
///
/// ```yaml
/// hooks:
///   user_defines:
///     just_jolt:
///       build: true
/// ```
///
/// Without it `Jolt.start` reports that the library is not in the build.
///
/// Android builds only arm64-v8a and x86_64: the library hands 64-bit handles
/// through pointer-sized finalizer tokens. On other ABIs nothing is bundled
/// either.
///
/// The dev flavour adds Jolt's debug renderer (`jj_debug_lines`) and its
/// asserts (reported on stderr, never a breakpoint): `dev: true` next to
/// `build: true`.
void main(List<String> args) async {
  await build(args, (input, output) async {
    if (!input.config.buildCodeAssets) return;
    if (input.userDefines['build'] != true) return;
    final code = input.config.code;
    final os = code.targetOS;
    final arch = code.targetArchitecture;
    if (os == OS.android &&
        arch != Architecture.arm64 &&
        arch != Architecture.x64) {
      return;
    }
    final dev = input.userDefines['dev'] == true;

    final root = input.packageRoot;
    final joltRoot = root.resolve('src/jolt/');
    final cmake = File.fromUri(joltRoot.resolve('Jolt/Jolt.cmake'));
    final unityDir = Directory.fromUri(input.outputDirectory.resolve('unity/'));
    final unity = unityFiles(joltSources(cmake.readAsStringSync()));

    final isMsvc = os == OS.windows;
    final isX64 = arch == Architecture.x64;

    final defines = <String, String?>{
      'JPH_OBJECT_LAYER_BITS': '32',
      if (isX64) 'JPH_USE_SSE4_1': null,
      if (isX64) 'JPH_USE_SSE4_2': null,
      if (dev) 'JPH_DEBUG_RENDERER': null,
      if (dev) 'JPH_ENABLE_ASSERTS': null,
    };

    final flags = <String>[
      if (isMsvc) ...['/GR-', '/fp:fast', '/bigobj', '/Zc:inline'],
      if (!isMsvc) ...[
        '-fno-rtti',
        '-fno-exceptions',
        '-fvisibility=hidden',
        '-pthread',
      ],
      if (!isMsvc && isX64) ...['-msse4.2', '-mpopcnt'],
      if (os == OS.macOS || os == OS.iOS) '-faligned-allocation',
      if (os == OS.android) '-Wno-unused-command-line-argument',
    ];

    final includes = [joltRoot.toFilePath()];

    final unityPaths = [
      for (final name in unity.keys)
        File.fromUri(unityDir.uri.resolve(name)).path,
    ];
    final staticOutput = BuildOutputBuilder();
    await _explainLongPaths(unityPaths, joltRoot, () async {
      final unitySources = writeUnityFiles(unityDir, unity);
      await CBuilder.library(
        name: 'jolt_core',
        assetName: 'jolt_core',
        sources: unitySources,
        includes: includes,
        defines: defines,
        flags: flags,
        std: 'c++17',
        language: Language.cpp,
        buildMode: BuildMode.release,
        linkModePreference: LinkModePreference.static,
      ).run(input: input, output: staticOutput);

      await CBuilder.library(
        name: 'just_jolt',
        assetName: 'just_jolt',
        sources: [root.resolve('src/jj/jj_capi.cpp').toFilePath()],
        includes: [
          ...includes,
          root.resolve('src/joltc/').toFilePath(),
          root.resolve('src/jj/').toFilePath(),
        ],
        defines: {
          ...defines,
          'JPH_SHARED_LIBRARY_BUILD': null,
          'JJ_BUILD': null,
        },
        flags: flags,
        std: 'c++17',
        language: Language.cpp,
        libraryDirectories: const ['.'],
        libraries: [
          'jolt_core',
          // Jolt calls libm (fmodf, …); Android and Linux need it linked
          // explicitly or the library fails to load.
          if (os == OS.android || os == OS.linux) 'm',
        ],
        cppLinkStdLib: os == OS.android ? 'c++_static' : null,
        buildMode: BuildMode.release,
      ).run(input: input, output: output);
    });
  });
}

/// The longest path Windows tools open: MSVC, and Dart's own directory
/// listing, are not long-path aware.
const _windowsMaxPath = 259;

/// Runs [build]. When it fails on Windows and a file it needs has a path
/// longer than [_windowsMaxPath], fails with a [BuildError] that says so,
/// rather than a "cannot find" for a file that is there.
Future<void> _explainLongPaths(
  List<String> unitySources,
  Uri joltRoot,
  Future<void> Function() build,
) async {
  try {
    await build();
  } on Exception catch (error, trace) {
    if (!Platform.isWindows) rethrow;
    String longest(Iterable<String> paths) =>
        paths.fold('', (a, b) => b.length > a.length ? b : a);
    final source = longest(unitySources);
    final header = longest(
      Directory.fromUri(
        joltRoot.resolve('Jolt/'),
      ).listSync(recursive: true).whereType<File>().map((f) => f.path),
    );
    final String where;
    final String path;
    if (source.length > _windowsMaxPath) {
      path = source;
      where = 'Move the project to a shorter folder';
    } else if (header.length > _windowsMaxPath) {
      path = header;
      where =
          'just_jolt itself is in a deep folder: move it, or point PUB_CACHE '
          'at a shorter one';
    } else {
      rethrow;
    }
    throw BuildError(
      message:
          'just_jolt: Windows build tools cannot open files whose path is '
          'longer than $_windowsMaxPath characters, and this build needs one of '
          '${path.length}: $path\n$where, then build again.',
      wrappedException: error,
      wrappedTrace: trace,
    );
  }
}
