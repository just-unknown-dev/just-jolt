// Jolt: the native library is in this package's own build (its pubspec
// asks for it), starts once, and counts its worlds.

@TestOn('vm')
library;

import 'dart:ffi';

import 'package:ffi/ffi.dart';
import 'package:just_jolt/ffi.dart';
import 'package:test/test.dart';

void main() {
  test('starts, and says so again without starting twice', () {
    expect(Jolt.start(), isNull);
    expect(Jolt.start(workerThreads: 1), isNull);
    expect(Jolt.isAvailable, isTrue);
  });

  test('is Jolt 5.6.0', () {
    expect(Jolt.version, '5.6.0');
  });

  test('counts live worlds and destroys them all', () {
    final before = Jolt.liveWorlds;
    final world = using(
      (Arena arena) => jj_world_create(arena<JJ_WorldSettings>()),
    );
    expect(world, isNot(0));
    expect(Jolt.liveWorlds, before + 1);
    Jolt.destroyAll();
    expect(Jolt.liveWorlds, 0);
    expect(jj_world_valid(world), isFalse);
  });
}
