// The native layer on its own: Jolt builds and initialises, and worlds,
// bodies, joints and characters can be torn down in any order. Ported from
// just_physics_engine's Box2D teardown cases: a hot restart finalizes
// everything at once, in whatever order the GC finds it, so a world can go
// before its own bodies. If one of these regresses the test runner crashes
// rather than failing.

@TestOn('vm')
library;

import 'dart:ffi';

import 'package:ffi/ffi.dart';
import 'package:test/test.dart';
import 'package:just_jolt/ffi.dart';

const _dt = 1 / 60;

int _world(Arena arena, {List<int>? masks}) {
  final s = arena<JJ_WorldSettings>();
  s.ref.gravity[1] = 9.81; // +Y is down.
  for (var i = 0; i < 16; i++) {
    s.ref.collides[i] = masks != null && i < masks.length ? masks[i] : 0xFFFF;
  }
  return jj_world_create(s);
}

Pointer<JPH_Shape> _boxShape(Arena arena, double h) {
  final half = arena<JPH_Vec3>();
  half.ref
    ..x = h
    ..y = h
    ..z = h;
  final settings = JPH_BoxShapeSettings_Create(half, 0.05);
  final shape = JPH_BoxShapeSettings_CreateShape(settings).cast<JPH_Shape>();
  JPH_ShapeSettings_Destroy(settings.cast());
  return shape;
}

int _body(
  Arena arena,
  int world,
  Pointer<JPH_Shape> shape, {
  double x = 0,
  double y = 0,
  int motion = JJ_MOTION_DYNAMIC,
}) {
  final s = arena<JJ_BodySettings>();
  s.ref
    ..position[0] = x
    ..position[1] = y
    ..rotation[3] = 1
    ..motionType = motion
    ..friction = 0.5
    ..gravityFactor = 1
    ..allowSleeping = true;
  return jj_body_create(world, shape, s);
}

int _hinge(Arena arena, int world, int a, int b) {
  final s = arena<JJ_JointSettings>();
  s.ref
    ..kind = JJ_JOINT_HINGE
    ..axis[1] = 1
    ..normal[0] = 1;
  return jj_joint_create(world, a, b, s);
}

int _character(Arena arena, int world) {
  final s = arena<JJ_CharacterSettings>();
  s.ref
    ..radius = 0.3
    ..halfHeight = 0.6
    ..up[1] = -1
    ..maxSlopeAngle = 0.87
    ..mass = 70
    ..maxStrength = 100
    ..innerBody = true;
  final p = arena<Float>(3);
  return jj_character_create(world, s, p);
}

void main() {
  setUpAll(() => expect(jj_init(-1), isTrue));
  tearDown(jj_destroy_all);

  test('the library is Jolt 5.6.0 and initialises again safely', () {
    expect(jj_version(), 0x050600);
    expect(jj_init(-1), isTrue);
  });

  test('create, step and destroy', () {
    using((arena) {
      final world = _world(arena);
      expect(world, isNot(0));
      expect(jj_world_valid(world), isTrue);
      expect(jj_live_worlds(), 1);
      final shape = _boxShape(arena, 0.5);
      final body = _body(arena, world, shape, y: -3);
      jj_shape_release(shape.cast()); // The body keeps its own reference.
      expect(jj_body_valid(body), isTrue);
      expect(jj_world_body_count(world), 1);
      for (var i = 0; i < 30; i++) {
        expect(jj_world_step(world, _dt, 1), 0);
      }
      final ids = arena<Int64>()..value = body;
      final out = arena<Float>(JJ_TRANSFORM_FLOATS);
      expect(jj_get_transforms(world, ids, 1, out), 1);
      expect(out[1], greaterThan(-3), reason: 'fell, +Y down');
      expect(out[8], greaterThan(0), reason: 'falling velocity');
      expect(out[13], 1, reason: 'awake');
      jj_world_destroy(world);
      expect(jj_world_valid(world), isFalse);
      expect(jj_body_valid(body), isFalse);
      expect(jj_live_worlds(), 0);
    });
  });

  test('a handle of one kind is not another kind', () {
    using((arena) {
      final world = _world(arena);
      final shape = _boxShape(arena, 0.5);
      final a = _body(arena, world, shape);
      final b = _body(arena, world, shape, x: 2);
      final joint = _hinge(arena, world, a, b);
      final character = _character(arena, world);
      jj_shape_release(shape.cast());
      expect(jj_joint_valid(joint), isTrue);
      expect(jj_body_valid(joint), isFalse);
      expect(jj_character_valid(joint), isFalse);
      expect(jj_joint_valid(a), isFalse);
      expect(jj_character_valid(character), isTrue);
      expect(jj_body_valid(character), isFalse);
      expect(jj_world_valid(a), isFalse);
      expect(
        (a >>> 32),
        jj_world_handle_tag(world),
        reason: 'body handles carry the tag',
      );
    });
  });

  group('teardown in any order', () {
    test('a world finalized before its bodies, joints and characters', () {
      using((arena) {
        final world = _world(arena);
        final shape = _boxShape(arena, 0.5);
        final bodies = [
          for (var i = 0; i < 8; i++) _body(arena, world, shape, x: i * 2.0),
        ];
        final joint = _hinge(arena, world, bodies[0], bodies[1]);
        final character = _character(arena, world);
        jj_world_step(world, _dt, 1);
        // The token a NativeFinalizer hands over is the handle.
        jj_world_finalize(Pointer.fromAddress(world));
        jj_joint_destroy(joint);
        jj_character_destroy(character);
        for (final b in bodies) {
          jj_body_destroy(b);
        }
        jj_shape_release(shape.cast()); // The shape outlives the world.
      });
    });

    test('bodies, joints and characters destroyed before their world', () {
      using((arena) {
        final world = _world(arena);
        final shape = _boxShape(arena, 0.5);
        final bodies = [
          for (var i = 0; i < 8; i++) _body(arena, world, shape, x: i * 2.0),
        ];
        jj_shape_release(shape.cast());
        final joint = _hinge(arena, world, bodies[0], bodies[1]);
        final character = _character(arena, world);
        jj_character_destroy(character);
        jj_joint_destroy(joint);
        for (final b in bodies) {
          jj_body_destroy(b);
        }
        expect(
          jj_world_body_count(world),
          0,
          reason: 'the inner body went too',
        );
        jj_world_destroy(world);
      });
    });

    test('everything destroyed twice', () {
      using((arena) {
        final world = _world(arena);
        final shape = _boxShape(arena, 0.5);
        final a = _body(arena, world, shape);
        final b = _body(arena, world, shape, x: 2);
        jj_shape_release(shape.cast());
        final joint = _hinge(arena, world, a, b);
        final character = _character(arena, world);
        jj_joint_destroy(joint);
        jj_joint_destroy(joint);
        jj_character_destroy(character);
        jj_character_destroy(character);
        jj_body_destroy(a);
        jj_body_destroy(a);
        jj_world_destroy(world);
        jj_world_destroy(world);
      });
    });

    test('a joint goes with one of its bodies', () {
      using((arena) {
        final world = _world(arena);
        final shape = _boxShape(arena, 0.5);
        final a = _body(arena, world, shape);
        final b = _body(arena, world, shape, x: 2);
        jj_shape_release(shape.cast());
        final joint = _hinge(arena, world, a, b);
        jj_body_destroy(a);
        expect(jj_joint_valid(joint), isFalse);
        jj_joint_destroy(joint);
        jj_world_step(world, _dt, 1);
        jj_world_destroy(world);
      });
    });

    test("a gone world's handles leave another world alone", () {
      using((arena) {
        final first = _world(arena);
        final second = _world(arena);
        final shape = _boxShape(arena, 0.5);
        final stale = _body(arena, first, shape);
        final live = _body(arena, second, shape);
        jj_shape_release(shape.cast());
        expect(
          stale & 0xFFFFFFFF,
          live & 0xFFFFFFFF,
          reason: 'the same BodyID in each world',
        );
        jj_world_finalize(Pointer.fromAddress(first));
        jj_body_destroy(stale);
        expect(jj_body_valid(live), isTrue);
        for (var i = 0; i < 10; i++) {
          jj_world_step(second, _dt, 1);
        }
        final ids = arena<Int64>()..value = live;
        final out = arena<Float>(JJ_TRANSFORM_FLOATS);
        jj_get_transforms(second, ids, 1, out);
        expect(out[1], greaterThan(0), reason: 'fell under gravity');
        // A body handle from another world reads as zeros.
        ids.value = stale;
        jj_get_transforms(second, ids, 1, out);
        expect(out[6], 0);
      });
    });

    test('a step, a query and a drain on a gone world do nothing', () {
      using((arena) {
        final world = _world(arena);
        final shape = _boxShape(arena, 0.5);
        _body(arena, world, shape);
        jj_world_destroy(world);
        expect(jj_world_step(world, _dt, 1), -1);
        final hits = arena<JJ_Hit>(4);
        final origin = arena<Float>(3);
        final dir = arena<Float>(3)..[1] = 10;
        expect(jj_raycast(world, origin, dir, 0xFFFF, 0, 0, hits, 4), 0);
        expect(
          jj_shape_cast(
            world,
            shape,
            arena<Float>(7)..[6] = 1,
            dir,
            0xFFFF,
            0,
            0,
            hits,
            4,
          ),
          0,
        );
        expect(jj_drain_events(world, arena<JJ_Event>(4), 4), 0);
        expect(
          jj_get_active_transforms(
            world,
            arena<Int64>(4),
            arena<Float>(4 * JJ_TRANSFORM_FLOATS),
            4,
          ),
          0,
        );
        jj_shape_release(shape.cast());
      });
    });

    test('200 worlds made and torn down, in both orders', () {
      using((arena) {
        final shape = _boxShape(arena, 0.5);
        for (var round = 0; round < 200; round++) {
          final world = _world(arena);
          final bodies = [
            for (var i = 0; i < 4; i++) _body(arena, world, shape, x: i * 2.0),
          ];
          final joint = _hinge(arena, world, bodies[0], bodies[1]);
          jj_world_step(world, _dt, 1);
          if (round.isEven) {
            jj_world_finalize(Pointer.fromAddress(world));
            jj_joint_destroy(joint);
            bodies.forEach(jj_body_destroy);
          } else {
            jj_joint_destroy(joint);
            bodies.forEach(jj_body_destroy);
            jj_world_destroy(world);
          }
        }
        jj_shape_release(shape.cast());
        expect(jj_live_worlds(), 0);
      });
    });

    test(
      'repeated hot restarts: destroy-all frees everything and old handles stay dead',
      () {
        using((arena) {
          final shape = _boxShape(arena, 0.5);
          var lastWorld = 0, lastBody = 0;
          for (var restart = 0; restart < 5; restart++) {
            final world = _world(arena);
            final body = _body(arena, world, shape);
            _character(arena, world);
            _hinge(arena, world, body, 0);
            jj_world_step(world, _dt, 1);
            expect(
              world,
              isNot(lastWorld),
              reason: 'a reused slot gets a new generation',
            );
            expect(jj_body_valid(lastBody), isFalse);
            jj_destroy_all();
            expect(jj_live_worlds(), 0);
            expect(jj_world_valid(world), isFalse);
            expect(jj_body_valid(body), isFalse);
            lastWorld = world;
            lastBody = body;
          }
          jj_shape_release(shape.cast());
        });
      },
    );
  });

  test('debug lines are there only in the dev flavour', () {
    using((arena) {
      final world = _world(arena);
      // An empty world draws nothing; without the renderer the answer is -1.
      expect(
        jj_debug_lines(world, 0, nullptr, 0),
        jj_has_debug_renderer() ? 0 : -1,
      );
    });
  });
}
