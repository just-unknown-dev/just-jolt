// The build hook compiles nothing unless the app's pubspec says
// `hooks: user_defines: just_jolt: build: true`, so an app that only has
// just_jolt as a dependency of a dependency neither builds nor ships Jolt.
// (With the switch on, the other tests here run on the library it builds.)

@TestOn('vm')
library;

import 'dart:io';

import 'package:code_assets/code_assets.dart';
import 'package:hooks/hooks.dart';
import 'package:test/test.dart';

import '../hook/build.dart' as hook;

Future<List<EncodedAsset>> _assets(Map<String, Object?> defines) async {
  late List<EncodedAsset> assets;
  await testCodeBuildHook(
    mainMethod: hook.main,
    userDefines: PackageUserDefines(
      workspacePubspec: PackageUserDefinesSource(
        defines: defines,
        basePath: Directory.current.uri,
      ),
    ),
    check: (input, output) => assets = output.assets.encodedAssets,
  );
  return assets;
}

void main() {
  test('nothing is built when the app does not ask', () async {
    expect(await _assets({}), isEmpty);
  });

  test('nothing is built when the app says build: false', () async {
    expect(await _assets({'build': false}), isEmpty);
  });

  test('dev alone builds nothing either', () async {
    expect(await _assets({'dev': true}), isEmpty);
  });
}
