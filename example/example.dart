// A ball dropped on a floor, through Jolt's C API.
//
// From this package's folder: `dart run example/example.dart`.
//
// In your own app, ask for the native library in your pubspec.yaml, or
// nothing is compiled and Jolt.start says so:
//
//   hooks:
//     user_defines:
//       just_jolt:
//         build: true

import 'dart:ffi';

import 'package:ffi/ffi.dart';
import 'package:just_jolt/ffi.dart';

void main() {
  final reason = Jolt.start();
  if (reason != null) {
    print('Jolt cannot run: $reason');
    return;
  }
  print('Jolt ${Jolt.version}');

  using((Arena arena) {
    // A world with +Y up, where every layer collides with every other.
    final world = arena<JJ_WorldSettings>();
    world.ref.gravity[1] = -9.81;
    for (var i = 0; i < JJ_LAYER_COUNT; i++) {
      world.ref.collides[i] = 0xFFFF;
    }
    final handle = jj_world_create(world);

    // Shapes come from joltc. A body takes its own reference, so release
    // ours once the bodies are made.
    final halfExtent = arena<JPH_Vec3>();
    halfExtent.ref
      ..x = 10
      ..y = 0.5
      ..z = 10;
    final boxSettings = JPH_BoxShapeSettings_Create(halfExtent, 0.05);
    final box = JPH_BoxShapeSettings_CreateShape(boxSettings).cast<JPH_Shape>();
    JPH_ShapeSettings_Destroy(boxSettings.cast());
    final sphere = JPH_SphereShape_Create(0.5).cast<JPH_Shape>();

    // A static floor whose top is at y = 0.
    final body = arena<JJ_BodySettings>();
    body.ref
      ..position[1] = -0.5
      ..rotation[3] = 1
      ..motionType = JJ_MOTION_STATIC
      ..friction = 0.5;
    jj_body_create(handle, box, body);

    // A ball 5 m above it. Settings start as zeros, so give it gravity.
    body.ref
      ..position[1] = 5
      ..motionType = JJ_MOTION_DYNAMIC
      ..restitution = 0.3
      ..gravityFactor = 1
      ..allowSleeping = true;
    final ball = jj_body_create(handle, sphere, body);
    jj_shape_release(box.cast());
    jj_shape_release(sphere.cast());

    // Two seconds at 60 steps a second, reading the ball back each half
    // second: x y z, the rotation, velocities, then whether it is awake.
    final ids = arena<Int64>()..value = ball;
    final pose = arena<Float>(JJ_TRANSFORM_FLOATS);
    for (var step = 1; step <= 120; step++) {
      jj_world_step(handle, 1 / 60, 1);
      if (step % 30 == 0) {
        jj_get_transforms(handle, ids, 1, pose);
        final height = pose[1].toStringAsFixed(2);
        final speed = pose[8].toStringAsFixed(2);
        print('${step / 60} s: height $height m, vertical speed $speed m/s');
      }
    }

    // Destroying the world destroys its bodies too.
    jj_world_destroy(handle);
  });
}
