// The web, compiled to JavaScript or to WebAssembly: there is no dart:ffi,
// so package:just_jolt/just_jolt.dart exports the stub, and Jolt says it
// cannot run instead of throwing. Run with
// `dart test -p chrome test/web_test.dart` (add `--compiler dart2wasm` for
// WebAssembly).

@TestOn('browser')
library;

import 'package:just_jolt/just_jolt.dart';
import 'package:test/test.dart';

void main() {
  test('Jolt says why it cannot run, and nothing throws', () {
    expect(Jolt.start(), contains('dart:ffi'));
    expect(Jolt.isAvailable, isFalse);
    expect(Jolt.version, isNull);
    expect(Jolt.hasDebugRenderer, isFalse);
    expect(Jolt.liveWorlds, 0);
    Jolt.destroyAll();
  });
}
