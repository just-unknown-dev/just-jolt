// JJ_WorldSettings.restitutionCombine: how a contact mixes its two bodies'
// bounciness. A ball that bounces fully (1) dropped on a floor that does
// not bounce at all (0).

@TestOn('vm')
library;

import 'dart:ffi';

import 'package:ffi/ffi.dart';
import 'package:just_jolt/ffi.dart';
import 'package:test/test.dart';

/// How fast the ball rises at its best after landing, m/s (+Y up here).
double _bestRise(int combine) => using((arena) {
  final w = arena<JJ_WorldSettings>();
  w.ref
    ..gravity[1] = -9.81
    ..restitutionCombine = combine;
  for (var i = 0; i < JJ_LAYER_COUNT; i++) {
    w.ref.collides[i] = 0xFFFF;
  }
  final world = jj_world_create(w);
  expect(world, isNot(0));
  try {
    final half = arena<JPH_Vec3>();
    half.ref
      ..x = 5
      ..y = 0.5
      ..z = 5;
    final boxSettings = JPH_BoxShapeSettings_Create(half, 0.05);
    final box = JPH_BoxShapeSettings_CreateShape(boxSettings).cast<JPH_Shape>();
    JPH_ShapeSettings_Destroy(boxSettings.cast());
    final ball = JPH_SphereShape_Create(0.5).cast<JPH_Shape>();

    final b = arena<JJ_BodySettings>();
    b.ref
      ..position[1] = -0.5
      ..rotation[3] = 1
      ..motionType = JJ_MOTION_STATIC
      ..restitution = 0;
    jj_body_create(world, box, b);
    b.ref
      ..position[1] = 2
      ..motionType = JJ_MOTION_DYNAMIC
      ..restitution = 1
      ..gravityFactor = 1;
    final body = jj_body_create(world, ball, b);
    jj_shape_release(box.cast());
    jj_shape_release(ball.cast());

    final ids = arena<Int64>()..value = body;
    final out = arena<Float>(JJ_TRANSFORM_FLOATS);
    var best = 0.0;
    var landed = false;
    for (var i = 0; i < 120; i++) {
      jj_world_step(world, 1 / 60, 1);
      jj_get_transforms(world, ids, 1, out);
      final vy = out[8];
      if (vy < -1) landed = true;
      if (landed && vy > best) best = vy;
    }
    expect(landed, isTrue, reason: 'the ball fell');
    return best;
  } finally {
    jj_world_destroy(world);
  }
});

void main() {
  setUpAll(() => expect(Jolt.start(), isNull));

  test("Jolt's own rule (the default) is the bouncier: the ball bounces", () {
    expect(_bestRise(JJ_COMBINE_DEFAULT), greaterThan(3));
  });

  test('max bounces as the default does', () {
    expect(_bestRise(JJ_COMBINE_MAX), greaterThan(3));
  });

  test('min is the less bouncy: the dead floor wins', () {
    expect(_bestRise(JJ_COMBINE_MIN), lessThan(0.5));
  });

  test('average bounces about half as high', () {
    final full = _bestRise(JJ_COMBINE_MAX);
    final half = _bestRise(JJ_COMBINE_AVERAGE);
    expect(half, greaterThan(0.5));
    expect(half, lessThan(full * 0.75));
  });
}
