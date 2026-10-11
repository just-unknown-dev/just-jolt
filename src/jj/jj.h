// just_jolt's own C layer over Jolt Physics.
//
// joltc (src/joltc/) exposes most of Jolt as a plain C API, and Dart uses it
// for shapes and per-body settings. This layer adds what joltc lacks or what
// cannot cross into Dart:
//   - worlds whose collision layers come from a 16x16 grid, each with its own
//     temporary allocator, all sharing one job system;
//   - contact, sensor and activation events, buffered here because Jolt calls
//     its listeners on worker threads in the middle of a step; Dart drains
//     them after the step;
//   - transforms read in batches (all moving bodies in one call);
//   - queries and the character controller, writing into caller-owned
//     buffers;
//   - joints, and safe teardown in any order.
//
// Handles are int64 and never reused. A world handle packs a slot and a
// generation; a body, joint or character handle also carries its world's
// slot and generation, so a handle from a destroyed world never resolves to
// a new one. Every function accepts dead handles and does nothing with them.
//
// Units are metres, seconds and radians. The layer assumes no axis: a world
// takes its gravity and a character its up. (A game whose +Y points down
// passes gravity (0, +9.81, 0) and up (0, -1, 0).)
#ifndef JJ_H_
#define JJ_H_

#include <stdbool.h>
#include <stdint.h>

#include "joltc.h"

#if defined(JJ_BUILD)
#  if defined(_MSC_VER)
#    define JJ_API __declspec(dllexport)
#  else
#    define JJ_API __attribute__((visibility("default")))
#  endif
#else
#  define JJ_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Constants ---------------------------------------------------------- */

/// Collision layers per world; a layer mask has one bit per layer.
#define JJ_LAYER_COUNT 16

/// Floats per body in jj_get_transforms and jj_get_active_transforms:
/// px py pz, qx qy qz qw, vx vy vz, wx wy wz, awake (0 or 1).
#define JJ_TRANSFORM_FLOATS 14

/// Floats per line in jj_debug_lines: x1 y1 z1 x2 y2 z2, colour (0xAARRGGBB
/// stored in the float's bits).
#define JJ_DEBUG_LINE_FLOATS 7

/// Motion types.
#define JJ_MOTION_STATIC 0
#define JJ_MOTION_KINEMATIC 1
#define JJ_MOTION_DYNAMIC 2

/// Event types (JJ_Event.type).
#define JJ_EVENT_CONTACT_BEGIN 1
#define JJ_EVENT_CONTACT_END 2
#define JJ_EVENT_SENSOR_BEGIN 3
#define JJ_EVENT_SENSOR_END 4
#define JJ_EVENT_CONTACT_STAY 5
#define JJ_EVENT_BODY_ACTIVATED 6
#define JJ_EVENT_BODY_DEACTIVATED 7

/// Query flags.
#define JJ_QUERY_ALL_HITS 1u      // every hit, nearest first; else the closest
#define JJ_QUERY_SENSORS 2u       // sensors can be hit too
#define JJ_QUERY_NO_CHARACTERS 4u // characters' bodies are passed over

/// Joint kinds (JJ_JointSettings.kind).
#define JJ_JOINT_FIXED 0
#define JJ_JOINT_POINT 1
#define JJ_JOINT_HINGE 2
#define JJ_JOINT_SLIDER 3
#define JJ_JOINT_DISTANCE 4
#define JJ_JOINT_SIX_DOF 5

/// Motor states (hinge and slider).
#define JJ_MOTOR_OFF 0
#define JJ_MOTOR_VELOCITY 1
#define JJ_MOTOR_POSITION 2

/// Character ground states (JJ_CharacterState.groundState), as Jolt's.
#define JJ_GROUND_ON_GROUND 0
#define JJ_GROUND_ON_STEEP_GROUND 1
#define JJ_GROUND_NOT_SUPPORTED 2
#define JJ_GROUND_IN_AIR 3

/// How two bodies' values mix into a contact's (JJ_WorldSettings).
#define JJ_COMBINE_DEFAULT 0 // Jolt's own rule
#define JJ_COMBINE_MIN 1     // the smaller of the two
#define JJ_COMBINE_MAX 2     // the larger of the two
#define JJ_COMBINE_AVERAGE 3 // halfway between

/* ---- Structs ------------------------------------------------------------ */

/// How a world is made.
typedef struct JJ_WorldSettings {
  /// Gravity in m/s^2.
  float gravity[3];
  /// The collision grid: bit j of `collides[i]` is set when layer i
  /// collides with layer j. A pair collides only when both rows say so.
  uint32_t collides[JJ_LAYER_COUNT];
  /// The most bodies the world holds; 0 picks 10240.
  int32_t maxBodies;
  /// The most body pairs the broad phase may find in a step; 0 picks
  /// 65536.
  int32_t maxBodyPairs;
  /// The most contact constraints in a step; 0 picks 10240.
  int32_t maxContactConstraints;
  /// Bytes of the world's temporary allocator; 0 picks 8 MB.
  int32_t tempAllocatorBytes;
  /// Whether touching pairs report JJ_EVENT_CONTACT_STAY after every step.
  bool reportContactStay;
  /// Whether bodies report JJ_EVENT_BODY_ACTIVATED / _DEACTIVATED.
  bool reportActivation;
  /// How two bodies' restitutions mix into a contact's: JJ_COMBINE_*.
  /// 0 (JJ_COMBINE_DEFAULT) keeps Jolt's own rule, the bouncier of the two.
  int32_t restitutionCombine;
} JJ_WorldSettings;

/// How a body is made.
typedef struct JJ_BodySettings {
  /// Where it starts, in metres.
  float position[3];
  /// Unit quaternion x y z w.
  float rotation[4];
  /// Starting velocity, m/s.
  float linearVelocity[3];
  /// Starting angular velocity, rad/s.
  float angularVelocity[3];
  /// JJ_MOTION_*.
  int32_t motionType;
  /// 0 discrete, 1 linear cast (continuous collision for fast bodies).
  int32_t motionQuality;
  /// Collision layer, 0-15.
  int32_t layer;
  /// Jolt's EAllowedDOFs bits; 0 means all six.
  int32_t allowedDofs;
  /// Friction coefficient. A contact mixes its two bodies' by Jolt's rule,
  /// the geometric mean.
  float friction;
  /// Bounciness, 0-1. A contact mixes its two bodies' as
  /// JJ_WorldSettings.restitutionCombine says.
  float restitution;
  /// How fast its motion dies away by itself.
  float linearDamping;
  /// How fast its spin dies away by itself.
  float angularDamping;
  /// How much of the world's gravity pulls it: 1 all, 0 none. Settings
  /// filled with zeros leave it at 0, so set it.
  float gravityFactor;
  /// Mass in kg; 0 or less computes it from the shape (density 1000).
  float mass;
  /// A sensor reports bodies that enter and leave it
  /// (JJ_EVENT_SENSOR_BEGIN / _END) and collides with nothing.
  bool isSensor;
  /// Whether it may fall asleep when it comes to rest.
  bool allowSleeping;
  /// Jolt's enhanced internal edge removal: smoother sliding over the
  /// inner edges of meshes and compound shapes, at some cost.
  bool enhancedInternalEdgeRemoval;
  /// Kept on the Jolt body as its user data.
  uint64_t userData;
} JJ_BodySettings;

/// A buffered event.
typedef struct JJ_Event {
  /// JJ_EVENT_*.
  int32_t type;
  /// The body the event is about, or the first of the two.
  int64_t bodyA;
  /// The second body; 0 for activation events.
  int64_t bodyB;
  /// Begin and stay events: a contact point.
  float point[3];
  /// Begin and stay events: the contact normal, from A to B. Zero on a
  /// sensor event.
  float normal[3];
  /// Begin and stay events: how deep the bodies overlap.
  float depth;
} JJ_Event;

/// A query hit.
typedef struct JJ_Hit {
  /// The body hit.
  int64_t body;
  /// Raycast and shape cast: how far along the cast, 0-1. Overlap: 0.
  float fraction;
  /// Where it was hit, in world space.
  float point[3];
  /// Surface normal at the hit, facing the query.
  float normal[3];
  /// Shape cast and overlap: penetration depth.
  float depth;
} JJ_Hit;

/// A joint between two bodies (body B may be the world). Anchors and axes
/// are in world space, as the bodies are when the joint is made.
typedef struct JJ_JointSettings {
  /// JJ_JOINT_*.
  int32_t kind;
  /// Anchor on body A.
  float pointA[3];
  /// Anchor on body B, or in the world when B is the world.
  float pointB[3];
  /// Hinge: the axis it turns about. Slider: the axis it slides along.
  /// Six-DOF: the twist (X) axis.
  float axis[3];
  /// Perpendicular to axis: where a hinge's angle is 0, and a six-DOF
  /// joint's Y axis.
  float normal[3];
  /// Whether a hinge, slider or distance joint keeps between limitMin and
  /// limitMax.
  bool hasLimits;
  /// The lower limit: radians for a hinge, metres for a slider or a
  /// distance joint.
  float limitMin;
  /// The upper limit, in the same units as limitMin.
  float limitMax;
  /// Makes a limit soft (on a distance joint, a spring): frequency in Hz,
  /// 0 for a rigid limit.
  float springFrequency;
  /// The soft limit's damping ratio.
  float springDamping;
  /// Hinge and slider: friction torque or force when no motor drives it.
  float maxFriction;
  /// Hinge and slider motor: JJ_MOTOR_*.
  int32_t motorState;
  /// The motor's target: a velocity (rad/s or m/s) or a position (radians
  /// or metres), as motorState says.
  float motorTarget;
  /// The most torque (hinge) or force (slider) the motor may use; 0 for no
  /// limit.
  float motorMaxForce;
  /// Six-DOF: the axes that move freely, bits 0-5 (translation X Y Z,
  /// rotation X Y Z). An axis neither free nor fixed is limited to
  /// sixDofMin..sixDofMax.
  uint32_t sixDofFree;
  /// Six-DOF: the axes that cannot move, bits as in sixDofFree. Fixed
  /// wins over free.
  uint32_t sixDofFixed;
  /// Six-DOF: the lower limit of each limited axis, metres or radians.
  float sixDofMin[6];
  /// Six-DOF: the upper limit of each limited axis, metres or radians.
  float sixDofMax[6];
} JJ_JointSettings;

/// How a character (Jolt's CharacterVirtual) is made. Its capsule stands on
/// its position: the bottom of the capsule is at the position.
typedef struct JJ_CharacterSettings {
  /// The capsule's radius, metres; above 0.
  float radius;
  /// Half the height of the capsule's cylinder part.
  float halfHeight;
  /// The world's up, normalised; zero means +Y, Jolt's own.
  float up[3];
  /// Steepest walkable slope, radians.
  float maxSlopeAngle;
  /// Mass in kg; 0 or less picks 70.
  float mass;
  /// The most force (N) it pushes bodies with; below 0 picks 100.
  float maxStrength;
  /// How far it keeps from geometry; 0 or less keeps Jolt's default.
  float padding;
  /// How far ahead it looks for contacts; 0 or less keeps Jolt's default.
  float predictiveContactDistance;
  /// 0-1: how much of a penetration is resolved per update.
  float penetrationRecoverySpeed;
  /// Collision layer, 0-15.
  int32_t layer;
  /// Whether it gets a kinematic body the rest of the world can see: sensors
  /// then detect it, and raycasts hit it.
  bool innerBody;
  /// As JJ_BodySettings.enhancedInternalEdgeRemoval.
  bool enhancedInternalEdgeRemoval;
  /// Kept on Jolt's CharacterVirtual as its user data.
  uint64_t userData;
} JJ_CharacterSettings;

/// What a character is doing after a step.
typedef struct JJ_CharacterState {
  /// Where its feet are.
  float position[3];
  /// Its velocity, m/s.
  float velocity[3];
  /// JJ_GROUND_*.
  int32_t groundState;
  /// The ground's normal where it touches, pointing away from the ground.
  float groundNormal[3];
  /// Velocity of the ground under it (a moving platform's).
  float groundVelocity[3];
  /// Where it touches the ground.
  float groundPosition[3];
  /// The body it stands on, or 0.
  int64_t groundBody;
  /// How many contacts jj_character_contacts would return.
  int32_t contactCount;
} JJ_CharacterState;

/// A character's contact.
typedef struct JJ_CharacterContact {
  /// The body touched, or 0 for another character.
  int64_t body;
  /// Another character touched, or 0.
  int64_t character;
  /// Where they touch.
  float position[3];
  /// Pointing towards the character.
  float normal[3];
  /// Velocity of the contact point.
  float velocity[3];
  /// Negative or zero when touching; positive is a predicted contact.
  float distance;
  /// Whether the body touched is a sensor.
  bool isSensor;
  /// Whether it actually collided (false for a predicted contact).
  bool hadCollision;
} JJ_CharacterContact;

/* ---- Library ------------------------------------------------------------ */

/// Jolt's version as (major << 16) | (minor << 8) | patch.
JJ_API uint32_t jj_version(void);

/// Initialises Jolt and the process-wide job system. Safe to call again;
/// the first call picks the thread count. workerThreads < 0 means "cores
/// minus one".
JJ_API bool jj_init(int32_t workerThreads);

/// Whether the library was built with the debug renderer (dev flavour).
JJ_API bool jj_has_debug_renderer(void);

/// Destroys every world, with its bodies, joints and characters. Used on hot
/// restart, when Dart state is gone but the native library stays loaded.
/// Shapes Dart still holds are not touched.
JJ_API void jj_destroy_all(void);

/// Number of live worlds (for tests and leak checks).
JJ_API int32_t jj_live_worlds(void);

/// Releases one reference to a joltc shape. Thread-safe: NativeFinalizer
/// calls it from any thread.
JJ_API void jj_shape_release(void* shape);

/* ---- Worlds ------------------------------------------------------------- */

/// Makes a world and returns its handle; 0 when jj_init has not run,
/// settings is null, or no more worlds fit.
JJ_API int64_t jj_world_create(const JJ_WorldSettings* settings);

/// Destroys a world with its bodies, joints and characters.
JJ_API void jj_world_destroy(int64_t world);

/// NativeFinalizer callback: the token is the world handle.
JJ_API void jj_world_finalize(void* token);

/// Whether world is alive.
JJ_API bool jj_world_valid(int64_t world);

/// The upper 32 bits every body, joint and character handle of this world
/// has; 0 for a dead world.
JJ_API uint32_t jj_world_handle_tag(int64_t world);

/// joltc's view of the world, for its plain API (the body interface); null
/// for a dead world.
JJ_API JPH_PhysicsSystem* jj_world_joltc(int64_t world);

/// Steps the world, then refreshes each character's ground velocity.
/// Returns Jolt's EPhysicsUpdateError bits (0 = fine), or -1 for a dead
/// world.
JJ_API int32_t jj_world_step(int64_t world, float dt, int32_t collisionSteps);

/// Sets the world's gravity: three floats, m/s^2.
JJ_API void jj_world_set_gravity(int64_t world, const float* gravity);

/// Rebuilds the broad phase; worth it after adding many static bodies.
JJ_API void jj_world_optimize(int64_t world);

/// How many bodies the world holds, characters' inner bodies included; 0
/// for a dead world.
JJ_API int32_t jj_world_body_count(int64_t world);

/* ---- Bodies ------------------------------------------------------------- */

/// Makes a body of [shape] (a joltc shape; the body takes its own
/// reference) and adds it to the world. Returns 0 when it cannot be made: a
/// dead world, a full world, a layer out of range, or a shape that must be
/// static (a mesh or height field) on a dynamic body.
JJ_API int64_t jj_body_create(int64_t world, const JPH_Shape* shape,
                              const JJ_BodySettings* settings);

/// Removes and destroys a body, and every joint holding it.
JJ_API void jj_body_destroy(int64_t body);

/// Whether body is alive.
JJ_API bool jj_body_valid(int64_t body);

/// Moves a body to another layer.
JJ_API void jj_body_set_layer(int64_t body, int32_t layer);

/// Writes JJ_TRANSFORM_FLOATS floats per body; dead handles write zeros.
/// Returns count, or 0 for a dead world.
JJ_API int32_t jj_get_transforms(int64_t world, const int64_t* bodies,
                                 int32_t count, float* out);

/// Writes the bodies that are moving (awake, not static), and those that
/// fell asleep since the last call, JJ_TRANSFORM_FLOATS floats each.
/// Characters' bodies are left out. Returns how many were written; when
/// capacity is too small, writes nothing and returns minus the number
/// needed.
JJ_API int32_t jj_get_active_transforms(int64_t world, int64_t* outBodies,
                                        float* out, int32_t capacity);

/* ---- Events ------------------------------------------------------------- */

/// Moves up to capacity buffered events into out, oldest first. Returns how
/// many; call again while it equals capacity.
JJ_API int32_t jj_drain_events(int64_t world, JJ_Event* out, int32_t capacity);

/// How many events are buffered, waiting for jj_drain_events.
JJ_API int32_t jj_pending_events(int64_t world);

/* ---- Queries ------------------------------------------------------------ */

/// Casts a ray from origin along direction (whose length is the ray's).
/// Hits bodies on layers in layerMask, except ignoreBody. Writes up to
/// capacity hits, nearest first; returns how many.
JJ_API int32_t jj_raycast(int64_t world, const float* origin,
                          const float* direction, uint32_t layerMask,
                          int64_t ignoreBody, uint32_t flags, JJ_Hit* out,
                          int32_t capacity);

/// Sweeps [shape] from pose (px py pz qx qy qz qw) along direction.
JJ_API int32_t jj_shape_cast(int64_t world, const JPH_Shape* shape,
                             const float* pose, const float* direction,
                             uint32_t layerMask, int64_t ignoreBody,
                             uint32_t flags, JJ_Hit* out, int32_t capacity);

/// The bodies [shape] at pose overlaps, one hit each (the deepest point).
JJ_API int32_t jj_overlap(int64_t world, const JPH_Shape* shape,
                          const float* pose, uint32_t layerMask,
                          int64_t ignoreBody, uint32_t flags, JJ_Hit* out,
                          int32_t capacity);

/* ---- Joints ------------------------------------------------------------- */

/// Joins bodyA to bodyB, or to the world when bodyB is 0. Returns 0 when
/// either body is dead or they are in different worlds.
JJ_API int64_t jj_joint_create(int64_t world, int64_t bodyA, int64_t bodyB,
                               const JJ_JointSettings* settings);

/// Removes and destroys a joint.
JJ_API void jj_joint_destroy(int64_t joint);

/// Whether joint is alive.
JJ_API bool jj_joint_valid(int64_t joint);

/// Turns a joint off (its bodies move as if it were not there) or back on.
JJ_API void jj_joint_set_enabled(int64_t joint, bool enabled);

/// Hinge and slider: JJ_MOTOR_* and its target (velocity or position).
JJ_API void jj_joint_set_motor(int64_t joint, int32_t state, float target);

/// Hinge: angle (radians). Slider: position (metres). Others: 0.
JJ_API float jj_joint_value(int64_t joint);

/* ---- Characters (Jolt CharacterVirtual) --------------------------------- */

/// Makes a character with its feet at position (three floats). Returns 0
/// for a dead world, a layer out of range, a radius that is not above 0,
/// or a negative halfHeight.
JJ_API int64_t jj_character_create(int64_t world,
                                   const JJ_CharacterSettings* settings,
                                   const float* position);

/// Destroys a character and its inner body.
JJ_API void jj_character_destroy(int64_t character);

/// Whether character is alive.
JJ_API bool jj_character_valid(int64_t character);

/// The character's body (see JJ_CharacterSettings.innerBody), or 0.
JJ_API int64_t jj_character_inner_body(int64_t character);

/// Sets the character's velocity, then moves it by one update:
/// CharacterVirtual::ExtendedUpdate, which slides along walls, sticks to the
/// floor (stepDown, pointing down) and climbs stairs (stepUp, pointing up).
/// A zero stepDown or stepUp turns that part off.
JJ_API void jj_character_step(int64_t character, float dt,
                              const float* velocity, const float* gravity,
                              const float* stepDown, const float* stepUp,
                              JJ_CharacterState* out);

/// The character's state without moving it.
JJ_API void jj_character_state(int64_t character, JJ_CharacterState* out);

/// Teleports the character; its velocity is kept.
JJ_API void jj_character_set_position(int64_t character, const float* position);

/// Writes up to capacity of the character's contacts from its last update;
/// returns how many.
JJ_API int32_t jj_character_contacts(int64_t character,
                                     JJ_CharacterContact* out,
                                     int32_t capacity);

/* ---- Debug lines (dev flavour) ------------------------------------------ */

/// Draws the world's bodies (and joints when flags & 1) as lines, writing
/// up to capacity of them, JJ_DEBUG_LINE_FLOATS floats each. Returns how
/// many there are in all (more than capacity: call again with more room),
/// or -1 when the library has no debug renderer.
JJ_API int32_t jj_debug_lines(int64_t world, uint32_t flags, float* out,
                              int32_t capacity);

#ifdef __cplusplus
}
#endif

#endif  // JJ_H_
