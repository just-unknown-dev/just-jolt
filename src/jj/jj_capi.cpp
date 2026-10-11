// The just_jolt native library: joltc and our jj_* layer in one translation
// unit, so the layer can use Jolt's C++ API and joltc's internals directly.
// See jj.h for the API.

#include "joltc.cpp"
#include "joltc_assert.cpp"

#include "jj.h"

#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceTable.h>
#include <Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilterTable.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/ObjectLayerPairFilterTable.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/Constraints/SixDOFConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>
#include <Jolt/Physics/PhysicsSystem.h>
#ifdef JPH_DEBUG_RENDERER
#include <Jolt/Renderer/DebugRendererSimple.h>
#endif

#ifdef _WIN32
#include <malloc.h>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace jj {

using namespace JPH;

// ---- Object layers -------------------------------------------------------
//
// A game names up to 16 collision layers and a grid of which pairs collide
// (assets/project/physics.json). Each body is also of a kind, which decides
// its broad-phase layer and what it may touch whatever the grid says:
//   static     never touches static
//   moving     dynamic and kinematic bodies
//   sensor     touches moving bodies and characters, never static or sensors
//   character  a character's inner body: touches moving bodies and sensors
// The object layer is kind * 16 + layer, 64 in all, and Jolt's table filters
// are built from the grid and the kinds once per world.

enum Kind : uint32 { kStatic = 0, kMoving = 1, kSensor = 2, kCharacter = 3 };
constexpr uint kKinds = 4;
constexpr uint kObjectLayers = kKinds * JJ_LAYER_COUNT;

inline ObjectLayer objectLayer(uint32 kind, uint32 layer) {
  return ObjectLayer(kind * JJ_LAYER_COUNT + (layer & 15u));
}
inline uint32 kindOf(ObjectLayer o) { return uint32(o) / JJ_LAYER_COUNT; }
inline uint32 layerOf(ObjectLayer o) { return uint32(o) % JJ_LAYER_COUNT; }

// How bouncy a contact is, for JJ_WorldSettings.restitutionCombine. Jolt's
// own rule (JJ_COMBINE_DEFAULT) is the bouncier of the two bodies.
static float minRestitution(const Body& a, const SubShapeID&, const Body& b,
                            const SubShapeID&) {
  return std::min(a.GetRestitution(), b.GetRestitution());
}

static float maxRestitution(const Body& a, const SubShapeID&, const Body& b,
                            const SubShapeID&) {
  return std::max(a.GetRestitution(), b.GetRestitution());
}

static float averageRestitution(const Body& a, const SubShapeID&, const Body& b,
                                const SubShapeID&) {
  return 0.5f * (a.GetRestitution() + b.GetRestitution());
}

static bool kindsTouch(uint32 a, uint32 b) {
  if (a > b) std::swap(a, b);
  switch (a) {
    case kStatic: return b == kMoving || b == kSensor;
    case kMoving: return b != kStatic;
    case kSensor: return b == kCharacter;
    default: return false;
  }
}

/// Query filter: a layer mask, and whether sensors and characters count.
class JjQueryFilter final : public ObjectLayerFilter {
 public:
  JjQueryFilter(uint32 mask, uint32 flags) : mask_(mask), flags_(flags) {}
  bool ShouldCollide(ObjectLayer o) const override {
    if ((mask_ & (1u << layerOf(o))) == 0) return false;
    switch (kindOf(o)) {
      case kSensor: return (flags_ & JJ_QUERY_SENSORS) != 0;
      case kCharacter: return (flags_ & JJ_QUERY_NO_CHARACTERS) == 0;
      default: return true;
    }
  }

 private:
  uint32 mask_;
  uint32 flags_;
};

/// What a character's capsule collides with: static and moving bodies on
/// the layers its own layer's grid row names.
class JjCharacterFilter final : public ObjectLayerFilter {
 public:
  explicit JjCharacterFilter(uint32 row) : row_(row) {}
  bool ShouldCollide(ObjectLayer o) const override {
    uint32 kind = kindOf(o);
    return (kind == kStatic || kind == kMoving) &&
           (row_ & (1u << layerOf(o))) != 0;
  }

 private:
  uint32 row_;
};

class JjIgnoreBody final : public BodyFilter {
 public:
  explicit JjIgnoreBody(BodyID id) : id_(id) {}
  bool ShouldCollide(const BodyID& id) const override { return id != id_; }
  bool ShouldCollideLocked(const Body& body) const override {
    return body.GetID() != id_;
  }

 private:
  BodyID id_;
};

// ---- Handles -------------------------------------------------------------
//
// world:   generation(32) << 32 | (slot + 1)
// object:  generation(14) << 50 | type(2) << 48 | (slot + 1)(16) << 32 | low
// For a body, low is its BodyID (index and sequence); for a joint or a
// character, sequence(12) << 20 | index(20) into the world's list. A world's
// generation is bumped each time its slot is reused, and an entry's sequence
// each time its index is.

enum HandleType : uint32 { kBodyHandle = 1, kJointHandle = 2, kCharacterHandle = 3 };
constexpr uint32 kGenMask = 0x3FFF;
constexpr uint32 kIndexBits = 20;
constexpr uint32 kIndexMask = (1u << kIndexBits) - 1;
constexpr uint32 kSeqMask = 0xFFF;

inline int64_t worldHandle(uint32 slot, uint32 gen) {
  return int64_t((uint64(gen) << 32) | uint64(slot + 1));
}
inline uint32 objectTag(uint32 slot, uint32 gen, HandleType type) {
  return ((gen & kGenMask) << 18) | (uint32(type) << 16) | ((slot + 1) & 0xFFFF);
}
inline int64_t objectHandle(uint32 tag, uint32 low) {
  return int64_t((uint64(tag) << 32) | uint64(low));
}
inline uint32 entryLow(uint32 index, uint32 seq) {
  return ((seq & kSeqMask) << kIndexBits) | (index & kIndexMask);
}

// ---- Worlds --------------------------------------------------------------

struct JjPairState {
  int count = 0;
  bool sensor = false;
  uint32 stayStep = 0;  // the step whose stay event was sent
  // Jolt drops the contacts of a body that falls asleep, and finds them
  // again when it wakes. To a game they never stopped touching: the pair is
  // kept, dormant, with no end event, and resumes without a begin. If its
  // bodies wake and it is not found again within a step, it really ended.
  bool dormant = false;
  int wakeChecks = 0;
};

struct JjCharacter {
  Ref<CharacterVirtual> character;
  uint32 layer = 0;
  uint32 row = 0;  // grid row of its layer
  uint32 seq = 0;
};

struct JjJoint {
  Ref<TwoBodyConstraint> constraint;
  BodyID a;
  BodyID b;  // invalid when fixed to the world
  int32_t kind = 0;
  uint32 seq = 0;
};

struct JjWorld;

class JjContacts final : public ContactListener {
 public:
  explicit JjContacts(JjWorld* world) : world_(world) {}
  void OnContactAdded(const Body& b1, const Body& b2,
                      const ContactManifold& manifold,
                      ContactSettings&) override;
  void OnContactPersisted(const Body& b1, const Body& b2,
                          const ContactManifold& manifold,
                          ContactSettings&) override;
  void OnContactRemoved(const SubShapeIDPair& pair) override;

 private:
  JjWorld* world_;
};

class JjActivation final : public BodyActivationListener {
 public:
  explicit JjActivation(JjWorld* world) : world_(world) {}
  void OnBodyActivated(const BodyID& id, uint64 userData) override;
  void OnBodyDeactivated(const BodyID& id, uint64 userData) override;

 private:
  JjWorld* world_;
};

struct JjWorld {
  uint32 slot = 0;
  uint32 gen = 0;
  uint32 collides[JJ_LAYER_COUNT] = {};
  bool reportStay = false;
  bool reportActivation = false;
  uint32 stepIndex = 0;

  ObjectLayerPairFilterTable* pairs = nullptr;
  BroadPhaseLayerInterfaceTable* broadPhase = nullptr;
  ObjectVsBroadPhaseLayerFilterTable* objectVsBroadPhase = nullptr;
  PhysicsSystem* system = nullptr;
  JPH_PhysicsSystem* joltc = nullptr;  // joltc's wrapper, for its C API
  TempAllocatorImplWithMallocFallback* temp = nullptr;
  JjContacts* contacts = nullptr;
  JjActivation* activation = nullptr;
  CharacterVsCharacterCollisionSimple characterVsCharacter;

  // Written by Jolt's threads during a step; read after it.
  std::mutex eventLock;
  std::vector<JJ_Event> events;
  std::unordered_map<uint64, JjPairState> pairStates;
  std::vector<BodyID> fellAsleep;
  std::unordered_set<uint32> sleeping;  // bodies asleep, by BodyID

  std::vector<JjCharacter*> characters;  // index; null when free
  std::vector<uint32> characterSeq;
  std::vector<JjJoint*> joints;  // index; null when free
  std::vector<uint32> jointSeq;
  BodyIDVector scratchIds;

  uint32 tag(HandleType type) const { return objectTag(slot, gen, type); }
  int64_t bodyHandle(BodyID id) const {
    return objectHandle(tag(kBodyHandle), id.GetIndexAndSequenceNumber());
  }
};

static uint64 pairKey(BodyID a, BodyID b) {
  uint32 x = a.GetIndexAndSequenceNumber();
  uint32 y = b.GetIndexAndSequenceNumber();
  if (x > y) std::swap(x, y);
  return (uint64(x) << 32) | y;
}

static void fillContact(JJ_Event& e, const ContactManifold& manifold, bool sensor) {
  if (manifold.mRelativeContactPointsOn1.size() > 0) {
    RVec3 p = manifold.GetWorldSpaceContactPointOn1(0);
    e.point[0] = float(p.GetX());
    e.point[1] = float(p.GetY());
    e.point[2] = float(p.GetZ());
  }
  if (!sensor) {
    e.normal[0] = manifold.mWorldSpaceNormal.GetX();
    e.normal[1] = manifold.mWorldSpaceNormal.GetY();
    e.normal[2] = manifold.mWorldSpaceNormal.GetZ();
  }
  e.depth = manifold.mPenetrationDepth;
}

void JjContacts::OnContactAdded(const Body& b1, const Body& b2,
                                const ContactManifold& manifold,
                                ContactSettings&) {
  std::lock_guard<std::mutex> guard(world_->eventLock);
  JjPairState& state = world_->pairStates[pairKey(b1.GetID(), b2.GetID())];
  // One begin per pair of bodies, however many sub-shapes touch.
  if (state.count++ > 0) return;
  if (state.dormant) {
    // Found again after a sleep: still touching, nothing to report.
    state.dormant = false;
    state.wakeChecks = 0;
    return;
  }
  state.sensor = b1.IsSensor() || b2.IsSensor();
  state.stayStep = world_->stepIndex;
  JJ_Event e{};
  e.type = state.sensor ? JJ_EVENT_SENSOR_BEGIN : JJ_EVENT_CONTACT_BEGIN;
  e.bodyA = world_->bodyHandle(b1.GetID());
  e.bodyB = world_->bodyHandle(b2.GetID());
  fillContact(e, manifold, state.sensor);
  world_->events.push_back(e);
}

void JjContacts::OnContactPersisted(const Body& b1, const Body& b2,
                                    const ContactManifold& manifold,
                                    ContactSettings&) {
  if (!world_->reportStay) return;
  std::lock_guard<std::mutex> guard(world_->eventLock);
  auto it = world_->pairStates.find(pairKey(b1.GetID(), b2.GetID()));
  if (it == world_->pairStates.end() || it->second.sensor) return;
  // One stay per pair per step.
  if (it->second.stayStep == world_->stepIndex) return;
  it->second.stayStep = world_->stepIndex;
  JJ_Event e{};
  e.type = JJ_EVENT_CONTACT_STAY;
  e.bodyA = world_->bodyHandle(b1.GetID());
  e.bodyB = world_->bodyHandle(b2.GetID());
  fillContact(e, manifold, false);
  world_->events.push_back(e);
}

void JjContacts::OnContactRemoved(const SubShapeIDPair& pair) {
  std::lock_guard<std::mutex> guard(world_->eventLock);
  auto it = world_->pairStates.find(pairKey(pair.GetBody1ID(), pair.GetBody2ID()));
  if (it == world_->pairStates.end()) return;
  if (--it->second.count > 0) return;
  auto& sleeping = world_->sleeping;
  if (sleeping.count(pair.GetBody1ID().GetIndexAndSequenceNumber()) != 0 ||
      sleeping.count(pair.GetBody2ID().GetIndexAndSequenceNumber()) != 0) {
    it->second.dormant = true;
    it->second.wakeChecks = 0;
    return;
  }
  JJ_Event e{};
  e.type = it->second.sensor ? JJ_EVENT_SENSOR_END : JJ_EVENT_CONTACT_END;
  e.bodyA = world_->bodyHandle(pair.GetBody1ID());
  e.bodyB = world_->bodyHandle(pair.GetBody2ID());
  world_->events.push_back(e);
  world_->pairStates.erase(it);
}

void JjActivation::OnBodyActivated(const BodyID& id, uint64) {
  std::lock_guard<std::mutex> guard(world_->eventLock);
  world_->sleeping.erase(id.GetIndexAndSequenceNumber());
  if (!world_->reportActivation) return;
  JJ_Event e{};
  e.type = JJ_EVENT_BODY_ACTIVATED;
  e.bodyA = world_->bodyHandle(id);
  world_->events.push_back(e);
}

void JjActivation::OnBodyDeactivated(const BodyID& id, uint64) {
  std::lock_guard<std::mutex> guard(world_->eventLock);
  // Its last pose still has to reach Dart (jj_get_active_transforms).
  world_->fellAsleep.push_back(id);
  world_->sleeping.insert(id.GetIndexAndSequenceNumber());
  if (!world_->reportActivation) return;
  JJ_Event e{};
  e.type = JJ_EVENT_BODY_DEACTIVATED;
  e.bodyA = world_->bodyHandle(id);
  world_->events.push_back(e);
}

// ---- Registry ------------------------------------------------------------

// Every entry point takes this lock: Dart finalizers may run on another
// thread, and a step must never overlap a destroy. The job system is made
// once, by jj_init, and lives as long as the process, so it is never torn
// down under a step either.
static std::recursive_mutex gLock;
static JobSystemThreadPool* gJobs = nullptr;
static std::vector<JjWorld*> gWorlds;  // index = slot; null when free
static std::vector<uint32> gGenerations;

#ifdef JPH_DEBUG_RENDERER
/// Collects what Jolt draws as lines.
class JjLineRenderer final : public DebugRendererSimple {
 public:
  std::vector<float> lines;
  void DrawLine(RVec3Arg from, RVec3Arg to, ColorArg color) override {
    uint32 argb = (uint32(color.a) << 24) | (uint32(color.r) << 16) |
                  (uint32(color.g) << 8) | uint32(color.b);
    float bits;
    std::memcpy(&bits, &argb, sizeof(bits));
    float l[JJ_DEBUG_LINE_FLOATS] = {
        float(from.GetX()), float(from.GetY()), float(from.GetZ()),
        float(to.GetX()),   float(to.GetY()),   float(to.GetZ()), bits};
    lines.insert(lines.end(), l, l + JJ_DEBUG_LINE_FLOATS);
  }
  void DrawText3D(RVec3Arg, const string_view&, ColorArg, float) override {}
};
static JjLineRenderer* gLines = nullptr;

/// Asserts in the dev flavour are reported, never a breakpoint: a game
/// should not stop in a native debugger for them.
static bool JjAssertFailed(const char* expression, const char* message,
                           const char* file, uint line) {
  std::fprintf(stderr, "just_jolt: Jolt assert %s:%u: (%s) %s\n", file, line,
               expression, message != nullptr ? message : "");
  return false;
}
#endif

// ---- Windows: the Dart profiler and the stack --------------------------------
//
// In debug and profile builds Dart's sampling profiler walks the native stack
// of the isolate's thread by following RBP, checking only that each frame
// pointer lies inside the thread's stack. MSVC's x64 code keeps no frame
// pointers, so while Jolt runs, RBP can hold any value; when it points at
// stack that was never used (reserved, not yet committed), the profiler's
// read crashes the process. So each thread that runs Jolt commits its stack
// below the caller once, keeping clear of the guard page and the overflow
// reserve. (Linux, Android and Apple stacks are mapped readable throughout.)
#ifdef _WIN32
static __declspec(noinline) void touchStack(size_t bytes) {
  // _alloca probes every page on the way down, committing each.
  volatile char* p = static_cast<volatile char*>(_alloca(bytes));
  p[0] = 0;
}

static void commitStack() {
  static thread_local bool committed = false;
  if (committed) return;
  committed = true;
  ULONG_PTR low = 0, high = 0;
  GetCurrentThreadStackLimits(&low, &high);
  volatile char here = 0;
  ULONG_PTR sp = reinterpret_cast<ULONG_PTR>(&here);
  const ULONG_PTR reserve = 128 * 1024;
  if (sp <= low + reserve) return;
  ULONG_PTR bytes = sp - low - reserve;
  const ULONG_PTR most = ULONG_PTR(16) << 20;
  touchStack(size_t(bytes < most ? bytes : most));
}
#else
static inline void commitStack() {}
#endif

static JjWorld* findWorld(int64_t handle) {
  uint64 h = uint64(handle);
  uint32 slot = uint32(h & 0xFFFFFFFF) - 1;
  uint32 gen = uint32(h >> 32);
  if (slot >= gWorlds.size()) return nullptr;
  JjWorld* w = gWorlds[slot];
  return (w != nullptr && w->gen == gen) ? w : nullptr;
}

/// Resolves an object handle of [type] to its world and low 32 bits.
static JjWorld* findOwner(int64_t handle, HandleType type, uint32* low) {
  uint64 h = uint64(handle);
  uint32 tag = uint32(h >> 32);
  uint32 slot = (tag & 0xFFFF) - 1;
  if (((tag >> 16) & 3) != uint32(type) || slot >= gWorlds.size()) return nullptr;
  JjWorld* w = gWorlds[slot];
  if (w == nullptr || w->tag(type) != tag) return nullptr;
  *low = uint32(h & 0xFFFFFFFF);
  return w;
}

static bool findBody(int64_t handle, JjWorld** world, BodyID* id) {
  uint32 low = 0;
  JjWorld* w = findOwner(handle, kBodyHandle, &low);
  if (w == nullptr) return false;
  BodyID body(low);
  if (!w->system->GetBodyInterfaceNoLock().IsAdded(body)) return false;
  *world = w;
  *id = body;
  return true;
}

static JjCharacter* findCharacter(int64_t handle, JjWorld** world, uint32* index) {
  uint32 low = 0;
  JjWorld* w = findOwner(handle, kCharacterHandle, &low);
  if (w == nullptr) return nullptr;
  uint32 i = low & kIndexMask;
  if (i >= w->characters.size()) return nullptr;
  JjCharacter* c = w->characters[i];
  if (c == nullptr || c->seq != (low >> kIndexBits)) return nullptr;
  *world = w;
  if (index != nullptr) *index = i;
  return c;
}

static JjJoint* findJoint(int64_t handle, JjWorld** world, uint32* index) {
  uint32 low = 0;
  JjWorld* w = findOwner(handle, kJointHandle, &low);
  if (w == nullptr) return nullptr;
  uint32 i = low & kIndexMask;
  if (i >= w->joints.size()) return nullptr;
  JjJoint* j = w->joints[i];
  if (j == nullptr || j->seq != (low >> kIndexBits)) return nullptr;
  *world = w;
  if (index != nullptr) *index = i;
  return j;
}

/// A free index in [list], growing it when there is none.
template <typename T>
static uint32 freeIndex(std::vector<T*>& list, std::vector<uint32>& seqs) {
  uint32 i = 0;
  while (i < list.size() && list[i] != nullptr) i++;
  if (i == list.size()) {
    list.push_back(nullptr);
    seqs.push_back(0);
  }
  return i;
}

static void removeJoint(JjWorld* w, uint32 index) {
  JjJoint* j = w->joints[index];
  if (j == nullptr) return;
  w->system->RemoveConstraint(j->constraint);
  w->joints[index] = nullptr;
  w->jointSeq[index] = (w->jointSeq[index] + 1) & kSeqMask;
  delete j;
}

static void removeCharacter(JjWorld* w, uint32 index) {
  JjCharacter* c = w->characters[index];
  if (c == nullptr) return;
  w->characterVsCharacter.Remove(c->character);
  w->characters[index] = nullptr;
  w->characterSeq[index] = (w->characterSeq[index] + 1) & kSeqMask;
  delete c;  // CharacterVirtual removes its inner body
}

/// Tears a world down in an order that is safe whatever state it is in:
/// listeners off, characters (their inner bodies need the system), joints
/// (they point at bodies), bodies, then the system and what it uses.
static void destroyWorld(JjWorld* w) {
  w->system->SetContactListener(nullptr);
  w->system->SetBodyActivationListener(nullptr);
  for (uint32 i = 0; i < w->characters.size(); i++) removeCharacter(w, i);
  for (uint32 i = 0; i < w->joints.size(); i++) removeJoint(w, i);

  BodyInterface& bodies = w->system->GetBodyInterfaceNoLock();
  BodyIDVector ids;
  w->system->GetBodies(ids);
  if (!ids.empty()) {
    bodies.RemoveBodies(ids.data(), int(ids.size()));
    bodies.DestroyBodies(ids.data(), int(ids.size()));
  }

  s_PhysicsSystems.erase(w->system);
  delete w->joltc;  // joltc's wrapper only; the system is deleted below
  delete w->system;
  delete w->objectVsBroadPhase;
  delete w->broadPhase;
  delete w->pairs;
  delete w->contacts;
  delete w->activation;
  delete w->temp;
  gWorlds[w->slot] = nullptr;
  delete w;
}

inline Vec3 vec3(const float* v) { return Vec3(v[0], v[1], v[2]); }
inline RVec3 rvec3(const float* v) { return RVec3(v[0], v[1], v[2]); }
inline void put3(float* o, Vec3Arg v) {
  o[0] = v.GetX();
  o[1] = v.GetY();
  o[2] = v.GetZ();
}
inline void putR3(float* o, RVec3Arg v) {
  o[0] = float(v.GetX());
  o[1] = float(v.GetY());
  o[2] = float(v.GetZ());
}

/// Quaternion x y z w, normalised; identity when it has no length.
inline Quat quat(const float* q) {
  Quat r(q[0], q[1], q[2], q[3]);
  return r.LengthSq() > 1.0e-12f ? r.Normalized() : Quat::sIdentity();
}

static void writeTransform(const Body& b, float* o) {
  putR3(o, b.GetPosition());
  Quat q = b.GetRotation();
  o[3] = q.GetX();
  o[4] = q.GetY();
  o[5] = q.GetZ();
  o[6] = q.GetW();
  if (b.IsStatic()) {
    for (int k = 7; k < 13; k++) o[k] = 0;
  } else {
    put3(o + 7, b.GetLinearVelocity());
    put3(o + 10, b.GetAngularVelocity());
  }
  o[13] = b.IsActive() ? 1.0f : 0.0f;
}

static void writeHit(JjWorld* w, JJ_Hit& h, BodyID body, float fraction,
                     RVec3Arg point, Vec3Arg normal, float depth) {
  h.body = w->bodyHandle(body);
  h.fraction = fraction;
  putR3(h.point, point);
  put3(h.normal, normal);
  h.depth = depth;
}

static void writeCharacterState(JjWorld* w, JjCharacter* c, JJ_CharacterState* out) {
  CharacterVirtual& ch = *c->character;
  putR3(out->position, ch.GetPosition());
  put3(out->velocity, ch.GetLinearVelocity());
  out->groundState = int32_t(ch.GetGroundState());
  put3(out->groundNormal, ch.GetGroundNormal());
  put3(out->groundVelocity, ch.GetGroundVelocity());
  putR3(out->groundPosition, ch.GetGroundPosition());
  BodyID ground = ch.GetGroundBodyID();
  out->groundBody = ground.IsInvalid() ? 0 : w->bodyHandle(ground);
  out->contactCount = int32_t(ch.GetActiveContacts().size());
}

/// Jolt's body 1 is our B (or the world) and its body 2 our A, so a hinge's
/// angle, a slider's position and their motor targets are A's, measured
/// from B.
static TwoBodyConstraintSettings* jointSettings(const JJ_JointSettings* s) {
  RVec3 pa = rvec3(s->pointB);  // Jolt's point 1: on B
  RVec3 pb = rvec3(s->pointA);  // Jolt's point 2: on A
  Vec3 axis = vec3(s->axis);
  Vec3 normal = vec3(s->normal);
  if (axis.LengthSq() < 1.0e-12f) axis = Vec3::sAxisX();
  axis = axis.Normalized();
  // The normal must be perpendicular to the axis; fix it up when it is not.
  normal = normal - axis * normal.Dot(axis);
  normal = normal.LengthSq() < 1.0e-12f ? axis.GetNormalizedPerpendicular()
                                        : normal.Normalized();
  SpringSettings spring(ESpringMode::FrequencyAndDamping, s->springFrequency,
                        s->springDamping);
  auto motor = [s](MotorSettings& m, bool linear) {
    if (s->motorMaxForce > 0) {
      if (linear) {
        m.SetForceLimit(s->motorMaxForce);
      } else {
        m.SetTorqueLimit(s->motorMaxForce);
      }
    }
  };

  switch (s->kind) {
    case JJ_JOINT_FIXED: {
      auto* f = new FixedConstraintSettings();
      f->mPoint1 = pa;
      f->mPoint2 = pb;
      return f;
    }
    case JJ_JOINT_POINT: {
      auto* p = new PointConstraintSettings();
      p->mPoint1 = pa;
      p->mPoint2 = pb;
      return p;
    }
    case JJ_JOINT_HINGE: {
      auto* h = new HingeConstraintSettings();
      h->mPoint1 = pa;
      h->mPoint2 = pb;
      h->mHingeAxis1 = h->mHingeAxis2 = axis;
      h->mNormalAxis1 = h->mNormalAxis2 = normal;
      if (s->hasLimits) {
        h->mLimitsMin = s->limitMin;
        h->mLimitsMax = s->limitMax;
        h->mLimitsSpringSettings = spring;
      }
      h->mMaxFrictionTorque = s->maxFriction;
      motor(h->mMotorSettings, false);
      return h;
    }
    case JJ_JOINT_SLIDER: {
      auto* l = new SliderConstraintSettings();
      l->mPoint1 = pa;
      l->mPoint2 = pb;
      l->mSliderAxis1 = l->mSliderAxis2 = axis;
      l->mNormalAxis1 = l->mNormalAxis2 = normal;
      if (s->hasLimits) {
        l->mLimitsMin = s->limitMin;
        l->mLimitsMax = s->limitMax;
        l->mLimitsSpringSettings = spring;
      }
      l->mMaxFrictionForce = s->maxFriction;
      motor(l->mMotorSettings, true);
      return l;
    }
    case JJ_JOINT_DISTANCE: {
      auto* d = new DistanceConstraintSettings();
      d->mPoint1 = pa;
      d->mPoint2 = pb;
      if (s->hasLimits) {
        d->mMinDistance = s->limitMin;
        d->mMaxDistance = s->limitMax;
      }
      d->mLimitsSpringSettings = spring;
      return d;
    }
    case JJ_JOINT_SIX_DOF: {
      auto* x = new SixDOFConstraintSettings();
      x->mPosition1 = pa;
      x->mPosition2 = pb;
      x->mAxisX1 = x->mAxisX2 = axis;
      x->mAxisY1 = x->mAxisY2 = normal;
      for (int a = 0; a < 6; a++) {
        auto e = SixDOFConstraintSettings::EAxis(a);
        if (s->sixDofFixed & (1u << a)) {
          x->MakeFixedAxis(e);
        } else if (s->sixDofFree & (1u << a)) {
          x->MakeFreeAxis(e);
        } else {
          x->SetLimitedAxis(e, s->sixDofMin[a], s->sixDofMax[a]);
        }
      }
      return x;
    }
    default:
      return nullptr;
  }
}

static void endPair(JjWorld* w, uint64 key, const JjPairState& state) {
  JJ_Event e{};
  e.type = state.sensor ? JJ_EVENT_SENSOR_END : JJ_EVENT_CONTACT_END;
  e.bodyA = w->bodyHandle(BodyID(uint32(key >> 32)));
  e.bodyB = w->bodyHandle(BodyID(uint32(key & 0xFFFFFFFF)));
  w->events.push_back(e);
}

/// Dormant pairs whose bodies are all awake again and were not found again
/// within a step have ended.
static void endWokenPairs(JjWorld* w) {
  std::lock_guard<std::mutex> events(w->eventLock);
  for (auto it = w->pairStates.begin(); it != w->pairStates.end();) {
    JjPairState& state = it->second;
    uint32 a = uint32(it->first >> 32), b = uint32(it->first & 0xFFFFFFFF);
    if (!state.dormant || w->sleeping.count(a) != 0 || w->sleeping.count(b) != 0 ||
        state.wakeChecks++ < 1) {
      ++it;
      continue;
    }
    endPair(w, it->first, state);
    it = w->pairStates.erase(it);
  }
}

/// A destroyed body's dormant pairs end now: Jolt already reported them gone.
static void endDormantPairsOf(JjWorld* w, BodyID id) {
  uint32 key = id.GetIndexAndSequenceNumber();
  std::lock_guard<std::mutex> events(w->eventLock);
  w->sleeping.erase(key);
  for (auto it = w->pairStates.begin(); it != w->pairStates.end();) {
    bool mine = uint32(it->first >> 32) == key || uint32(it->first & 0xFFFFFFFF) == key;
    if (mine && it->second.dormant) {
      endPair(w, it->first, it->second);
      it = w->pairStates.erase(it);
    } else {
      ++it;
    }
  }
}

/// Wakes a joint's bodies, so a change to it takes effect.
static void wakeJoint(JjWorld* w, JjJoint* j) {
  BodyInterface& bodies = w->system->GetBodyInterface();
  bodies.ActivateBody(j->a);
  if (!j->b.IsInvalid()) bodies.ActivateBody(j->b);
}

static void applyMotor(JjJoint* j, int32_t state, float target) {
  EMotorState motor = state == JJ_MOTOR_VELOCITY   ? EMotorState::Velocity
                      : state == JJ_MOTOR_POSITION ? EMotorState::Position
                                                   : EMotorState::Off;
  if (j->kind == JJ_JOINT_HINGE) {
    auto* h = static_cast<HingeConstraint*>(j->constraint.GetPtr());
    if (motor == EMotorState::Velocity) h->SetTargetAngularVelocity(target);
    if (motor == EMotorState::Position) h->SetTargetAngle(target);
    h->SetMotorState(motor);
  } else if (j->kind == JJ_JOINT_SLIDER) {
    auto* l = static_cast<SliderConstraint*>(j->constraint.GetPtr());
    if (motor == EMotorState::Velocity) l->SetTargetVelocity(target);
    if (motor == EMotorState::Position) l->SetTargetPosition(target);
    l->SetMotorState(motor);
  }
}

}  // namespace jj

using namespace jj;

extern "C" {

/* ---- Library ------------------------------------------------------------ */

uint32_t jj_version(void) {
  return (JPH_VERSION_MAJOR << 16) | (JPH_VERSION_MINOR << 8) | JPH_VERSION_PATCH;
}

bool jj_init(int32_t workerThreads) {
  std::lock_guard<std::recursive_mutex> guard(gLock);
  commitStack();
  if (!JPH_Init()) return false;
#ifdef JPH_DEBUG_RENDERER
  JPH::AssertFailed = JjAssertFailed;
#endif
  if (gJobs == nullptr) {
    int threads = workerThreads >= 0
                      ? workerThreads
                      : int(std::thread::hardware_concurrency()) - 1;
    if (threads < 0) threads = 0;
    gJobs = new JobSystemThreadPool(cMaxPhysicsJobs, cMaxPhysicsBarriers, threads);
  }
  return true;
}

bool jj_has_debug_renderer(void) {
#ifdef JPH_DEBUG_RENDERER
  return true;
#else
  return false;
#endif
}

void jj_destroy_all(void) {
  std::lock_guard<std::recursive_mutex> guard(gLock);
  for (JjWorld* w : gWorlds) {
    if (w != nullptr) destroyWorld(w);
  }
}

int32_t jj_live_worlds(void) {
  std::lock_guard<std::recursive_mutex> guard(gLock);
  int32_t n = 0;
  for (JjWorld* w : gWorlds) n += w != nullptr ? 1 : 0;
  return n;
}

void jj_shape_release(void* shape) {
  std::lock_guard<std::recursive_mutex> guard(gLock);
  if (shape != nullptr) reinterpret_cast<Shape*>(shape)->Release();
}

/* ---- Worlds ------------------------------------------------------------- */

int64_t jj_world_create(const JJ_WorldSettings* s) {
  std::lock_guard<std::recursive_mutex> guard(gLock);
  commitStack();
  if (gJobs == nullptr || s == nullptr) return 0;

  uint32 slot = 0;
  while (slot < gWorlds.size() && gWorlds[slot] != nullptr) slot++;
  if (slot == gWorlds.size()) {
    if (slot >= 0xFFFE) return 0;
    gWorlds.push_back(nullptr);
    gGenerations.push_back(0);
  }

  JjWorld* w = new JjWorld();
  w->slot = slot;
  w->gen = ++gGenerations[slot];
  w->reportStay = s->reportContactStay;
  w->reportActivation = s->reportActivation;
  for (int i = 0; i < JJ_LAYER_COUNT; i++) w->collides[i] = s->collides[i] & 0xFFFF;

  // The grid is symmetric however it was given: a pair collides only when
  // both rows say so.
  w->pairs = new ObjectLayerPairFilterTable(kObjectLayers);
  w->broadPhase = new BroadPhaseLayerInterfaceTable(kObjectLayers, kKinds);
  for (uint32 a = 0; a < kObjectLayers; a++) {
    w->broadPhase->MapObjectToBroadPhaseLayer(ObjectLayer(a),
                                              BroadPhaseLayer(uint8(kindOf(ObjectLayer(a)))));
    for (uint32 b = a; b < kObjectLayers; b++) {
      uint32 la = layerOf(ObjectLayer(a));
      uint32 lb = layerOf(ObjectLayer(b));
      bool grid = (w->collides[la] & (1u << lb)) != 0 &&
                  (w->collides[lb] & (1u << la)) != 0;
      if (grid && kindsTouch(kindOf(ObjectLayer(a)), kindOf(ObjectLayer(b)))) {
        w->pairs->EnableCollision(ObjectLayer(a), ObjectLayer(b));
      }
    }
  }
  w->objectVsBroadPhase = new ObjectVsBroadPhaseLayerFilterTable(
      *w->broadPhase, kKinds, *w->pairs, kObjectLayers);

  int32_t tempBytes = s->tempAllocatorBytes > 0 ? s->tempAllocatorBytes : 8 * 1024 * 1024;
  w->temp = new TempAllocatorImplWithMallocFallback(uint(tempBytes));
  w->system = new PhysicsSystem();
  w->system->Init(s->maxBodies > 0 ? uint(s->maxBodies) : 10240, 0,
                  s->maxBodyPairs > 0 ? uint(s->maxBodyPairs) : 65536,
                  s->maxContactConstraints > 0 ? uint(s->maxContactConstraints) : 10240,
                  *w->broadPhase, *w->objectVsBroadPhase, *w->pairs);
  w->system->SetGravity(vec3(s->gravity));
  switch (s->restitutionCombine) {
    case JJ_COMBINE_MIN: w->system->SetCombineRestitution(minRestitution); break;
    case JJ_COMBINE_MAX: w->system->SetCombineRestitution(maxRestitution); break;
    case JJ_COMBINE_AVERAGE: w->system->SetCombineRestitution(averageRestitution); break;
    default: break; // Jolt's own rule.
  }
  w->contacts = new JjContacts(w);
  w->activation = new JjActivation(w);
  w->system->SetContactListener(w->contacts);
  w->system->SetBodyActivationListener(w->activation);

  // Register with joltc so its C API (the body interface) works on this
  // world too.
  w->joltc = new JPH_PhysicsSystem();
  w->joltc->broadPhaseLayerInterface = w->broadPhase;
  w->joltc->objectLayerPairFilter = w->pairs;
  w->joltc->objectVsBroadPhaseLayerFilter = w->objectVsBroadPhase;
  w->joltc->physicsSystem = w->system;
  s_PhysicsSystems[w->system] = w->joltc;

  gWorlds[slot] = w;
  return worldHandle(slot, w->gen);
}

void jj_world_destroy(int64_t world) {
  std::lock_guard<std::recursive_mutex> guard(gLock);
  if (JjWorld* w = findWorld(world)) destroyWorld(w);
}

void jj_world_finalize(void* token) {
  jj_world_destroy(int64_t(reinterpret_cast<intptr_t>(token)));
}

bool jj_world_valid(int64_t world) {
  std::lock_guard<std::recursive_mutex> guard(gLock);
  return findWorld(world) != nullptr;
}

uint32_t jj_world_handle_tag(int64_t world) {
  std::lock_guard<std::recursive_mutex> guard(gLock);
  JjWorld* w = findWorld(world);
  return w == nullptr ? 0 : w->tag(kBodyHandle);
}

JPH_PhysicsSystem* jj_world_joltc(int64_t world) {
  std::lock_guard<std::recursive_mutex> guard(gLock);
  JjWorld* w = findWorld(world);
  return w == nullptr ? nullptr : w->joltc;
}

int32_t jj_world_step(int64_t world, float dt, int32_t collisionSteps) {
  std::lock_guard<std::recursive_mutex> guard(gLock);
  commitStack();
  JjWorld* w = findWorld(world);
  if (w == nullptr) return -1;
  {
    std::lock_guard<std::mutex> events(w->eventLock);
    w->stepIndex++;
  }
  int32_t error = int32_t(w->system->Update(
      dt, collisionSteps < 1 ? 1 : collisionSteps, w->temp, gJobs));
  endWokenPairs(w);
  // What a character stands on may have moved; the next velocity it is
  // given should know.
  for (JjCharacter* c : w->characters) {
    if (c != nullptr) c->character->UpdateGroundVelocity();
  }
  return error;
}

void jj_world_set_gravity(int64_t world, const float* gravity) {
  std::lock_guard<std::recursive_mutex> guard(gLock);
  if (JjWorld* w = findWorld(world)) w->system->SetGravity(vec3(gravity));
}

void jj_world_optimize(int64_t world) {
  std::lock_guard<std::recursive_mutex> guard(gLock);
  if (JjWorld* w = findWorld(world)) w->system->OptimizeBroadPhase();
}

int32_t jj_world_body_count(int64_t world) {
  std::lock_guard<std::recursive_mutex> guard(gLock);
  JjWorld* w = findWorld(world);
  return w == nullptr ? 0 : int32_t(w->system->GetNumBodies());
}

/* ---- Bodies ------------------------------------------------------------- */

int64_t jj_body_create(int64_t world, const JPH_Shape* shapePtr,
                       const JJ_BodySettings* s) {
  std::lock_guard<std::recursive_mutex> guard(gLock);
  commitStack();
  JjWorld* w = findWorld(world);
  if (w == nullptr || shapePtr == nullptr || s == nullptr) return 0;
  if (s->layer < 0 || s->layer >= JJ_LAYER_COUNT) return 0;
  const Shape* shape = reinterpret_cast<const Shape*>(shapePtr);

  EMotionType type = s->motionType == JJ_MOTION_STATIC      ? EMotionType::Static
                     : s->motionType == JJ_MOTION_KINEMATIC ? EMotionType::Kinematic
                                                            : EMotionType::Dynamic;
  if (type != EMotionType::Static && shape->MustBeStatic()) return 0;
  uint32 kind = s->isSensor ? kSensor : type == EMotionType::Static ? kStatic : kMoving;

  BodyCreationSettings b(shape, rvec3(s->position), quat(s->rotation), type,
                         objectLayer(kind, uint32(s->layer)));
  b.mLinearVelocity = vec3(s->linearVelocity);
  b.mAngularVelocity = vec3(s->angularVelocity);
  b.mMotionQuality = s->motionQuality == 1 ? EMotionQuality::LinearCast
                                           : EMotionQuality::Discrete;
  if (s->allowedDofs != 0) b.mAllowedDOFs = EAllowedDOFs(uint8(s->allowedDofs & 0x3F));
  b.mFriction = s->friction;
  b.mRestitution = s->restitution;
  b.mLinearDamping = s->linearDamping;
  b.mAngularDamping = s->angularDamping;
  b.mGravityFactor = s->gravityFactor;
  b.mIsSensor = s->isSensor;
  b.mAllowSleeping = s->allowSleeping;
  b.mEnhancedInternalEdgeRemoval = s->enhancedInternalEdgeRemoval;
  b.mUserData = s->userData;
  if (type != EMotionType::Static) {
    MassProperties mass = shape->GetMassProperties();
    if (!(mass.mMass > 0.0f) || !std::isfinite(mass.mMass)) {
      // A shape with no volume (a mesh): give it the inertia of its bounds.
      AABox bounds = shape->GetLocalBounds();
      Vec3 size = Vec3::sMax(bounds.GetSize(), Vec3::sReplicate(0.01f));
      b.mOverrideMassProperties = EOverrideMassProperties::MassAndInertiaProvided;
      b.mMassPropertiesOverride.SetMassAndInertiaOfSolidBox(size, 1000.0f);
      if (s->mass > 0) b.mMassPropertiesOverride.ScaleToMass(s->mass);
    } else if (s->mass > 0) {
      b.mOverrideMassProperties = EOverrideMassProperties::CalculateInertia;
      b.mMassPropertiesOverride.mMass = s->mass;
    }
  }

  BodyID id = w->system->GetBodyInterface().CreateAndAddBody(
      b, type == EMotionType::Static ? EActivation::DontActivate : EActivation::Activate);
  if (id.IsInvalid()) return 0;
  return w->bodyHandle(id);
}

void jj_body_destroy(int64_t body) {
  std::lock_guard<std::recursive_mutex> guard(gLock);
  JjWorld* w = nullptr;
  BodyID id;
  if (!findBody(body, &w, &id)) return;
  // Its joints first: a joint must never outlive a body it holds.
  for (uint32 i = 0; i < w->joints.size(); i++) {
    JjJoint* j = w->joints[i];
    if (j != nullptr && (j->a == id || j->b == id)) removeJoint(w, i);
  }
  BodyInterface& bodies = w->system->GetBodyInterface();
  bodies.RemoveBody(id);
  bodies.DestroyBody(id);
  endDormantPairsOf(w, id);
}

bool jj_body_valid(int64_t body) {
  std::lock_guard<std::recursive_mutex> guard(gLock);
  JjWorld* w = nullptr;
  BodyID id;
  return findBody(body, &w, &id);
}

void jj_body_set_layer(int64_t body, int32_t layer) {
  std::lock_guard<std::recursive_mutex> guard(gLock);
  JjWorld* w = nullptr;
  BodyID id;
  if (layer < 0 || layer >= JJ_LAYER_COUNT || !findBody(body, &w, &id)) return;
  BodyInterface& bodies = w->system->GetBodyInterface();
  ObjectLayer old = bodies.GetObjectLayer(id);
  bodies.SetObjectLayer(id, objectLayer(kindOf(old), uint32(layer)));
}

int32_t jj_get_transforms(int64_t world, const int64_t* bodies, int32_t count,
                          float* out) {
  std::lock_guard<std::recursive_mutex> guard(gLock);
  JjWorld* w = findWorld(world);
  if (w == nullptr) return 0;
  const BodyLockInterfaceNoLock& locks = w->system->GetBodyLockInterfaceNoLock();
  for (int32_t i = 0; i < count; i++) {
    float* o = out + i * JJ_TRANSFORM_FLOATS;
    JjWorld* owner = nullptr;
    BodyID id;
    if (!findBody(bodies[i], &owner, &id) || owner != w) {
      std::memset(o, 0, sizeof(float) * JJ_TRANSFORM_FLOATS);
      continue;
    }
    BodyLockRead lock(locks, id);
    if (lock.Succeeded()) writeTransform(lock.GetBody(), o);
  }
  return count;
}

int32_t jj_get_active_transforms(int64_t world, int64_t* outBodies, float* out,
                                 int32_t capacity) {
  std::lock_guard<std::recursive_mutex> guard(gLock);
  JjWorld* w = findWorld(world);
  if (w == nullptr) return 0;
  BodyIDVector& ids = w->scratchIds;
  w->system->GetActiveBodies(EBodyType::RigidBody, ids);
  std::lock_guard<std::mutex> events(w->eventLock);
  size_t total = ids.size() + w->fellAsleep.size();
  if (total > size_t(capacity)) return -int32_t(total);
  ids.insert(ids.end(), w->fellAsleep.begin(), w->fellAsleep.end());
  w->fellAsleep.clear();

  const BodyLockInterfaceNoLock& locks = w->system->GetBodyLockInterfaceNoLock();
  int32_t n = 0;
  for (const BodyID& id : ids) {
    BodyLockRead lock(locks, id);
    if (!lock.Succeeded()) continue;  // destroyed since it fell asleep
    const Body& body = lock.GetBody();
    if (!body.IsInBroadPhase() || kindOf(body.GetObjectLayer()) == kCharacter) continue;
    outBodies[n] = w->bodyHandle(id);
    writeTransform(body, out + n * JJ_TRANSFORM_FLOATS);
    n++;
  }
  return n;
}

/* ---- Events ------------------------------------------------------------- */

int32_t jj_drain_events(int64_t world, JJ_Event* out, int32_t capacity) {
  std::lock_guard<std::recursive_mutex> guard(gLock);
  JjWorld* w = findWorld(world);
  if (w == nullptr || capacity <= 0) return 0;
  std::lock_guard<std::mutex> events(w->eventLock);
  int32_t n = int32_t(w->events.size()) < capacity ? int32_t(w->events.size())
                                                   : capacity;
  if (n > 0) {
    std::memcpy(out, w->events.data(), sizeof(JJ_Event) * size_t(n));
    w->events.erase(w->events.begin(), w->events.begin() + n);
  }
  return n;
}

int32_t jj_pending_events(int64_t world) {
  std::lock_guard<std::recursive_mutex> guard(gLock);
  JjWorld* w = findWorld(world);
  if (w == nullptr) return 0;
  std::lock_guard<std::mutex> events(w->eventLock);
  return int32_t(w->events.size());
}

/* ---- Queries ------------------------------------------------------------ */

int32_t jj_raycast(int64_t world, const float* origin, const float* direction,
                   uint32_t layerMask, int64_t ignoreBody, uint32_t flags,
                   JJ_Hit* out, int32_t capacity) {
  std::lock_guard<std::recursive_mutex> guard(gLock);
  commitStack();
  JjWorld* w = findWorld(world);
  if (w == nullptr || capacity <= 0) return 0;

  BodyID ignored;
  JjWorld* ignoreOwner = nullptr;
  findBody(ignoreBody, &ignoreOwner, &ignored);
  JjIgnoreBody bodyFilter(ignored);
  JjQueryFilter layerFilter(layerMask, flags);
  RRayCast ray{rvec3(origin), vec3(direction)};
  const NarrowPhaseQuery& query = w->system->GetNarrowPhaseQuery();

  std::vector<RayCastResult> hits;
  if ((flags & JJ_QUERY_ALL_HITS) != 0) {
    AllHitCollisionCollector<CastRayCollector> collector;
    query.CastRay(ray, RayCastSettings(), collector, BroadPhaseLayerFilter(),
                  layerFilter, bodyFilter);
    collector.Sort();
    hits.assign(collector.mHits.begin(), collector.mHits.end());
  } else {
    RayCastResult hit;
    if (query.CastRay(ray, hit, BroadPhaseLayerFilter(), layerFilter, bodyFilter)) {
      hits.push_back(hit);
    }
  }

  const BodyLockInterfaceNoLock& locks = w->system->GetBodyLockInterfaceNoLock();
  int32_t n = 0;
  for (const RayCastResult& hit : hits) {
    if (n >= capacity) break;
    RVec3 point = ray.GetPointOnRay(hit.mFraction);
    Vec3 normal = Vec3::sZero();
    BodyLockRead lock(locks, hit.mBodyID);
    if (lock.Succeeded()) {
      normal = lock.GetBody().GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, point);
    }
    writeHit(w, out[n++], hit.mBodyID, hit.mFraction, point, normal, 0);
  }
  return n;
}

int32_t jj_shape_cast(int64_t world, const JPH_Shape* shapePtr, const float* pose,
                      const float* direction, uint32_t layerMask,
                      int64_t ignoreBody, uint32_t flags, JJ_Hit* out,
                      int32_t capacity) {
  std::lock_guard<std::recursive_mutex> guard(gLock);
  commitStack();
  JjWorld* w = findWorld(world);
  if (w == nullptr || shapePtr == nullptr || capacity <= 0) return 0;
  const Shape* shape = reinterpret_cast<const Shape*>(shapePtr);

  BodyID ignored;
  JjWorld* ignoreOwner = nullptr;
  findBody(ignoreBody, &ignoreOwner, &ignored);
  JjIgnoreBody bodyFilter(ignored);
  JjQueryFilter layerFilter(layerMask, flags);
  RMat44 start = RMat44::sRotationTranslation(quat(pose + 3), rvec3(pose));
  RShapeCast cast = RShapeCast::sFromWorldTransform(shape, Vec3::sOne(), start,
                                                    vec3(direction));
  ShapeCastSettings settings;
  settings.mReturnDeepestPoint = true;
  settings.mBackFaceModeTriangles = EBackFaceMode::IgnoreBackFaces;
  const NarrowPhaseQuery& query = w->system->GetNarrowPhaseQuery();

  std::vector<ShapeCastResult> hits;
  if ((flags & JJ_QUERY_ALL_HITS) != 0) {
    AllHitCollisionCollector<CastShapeCollector> collector;
    query.CastShape(cast, settings, RVec3::sZero(), collector,
                    BroadPhaseLayerFilter(), layerFilter, bodyFilter);
    collector.Sort();
    hits.assign(collector.mHits.begin(), collector.mHits.end());
  } else {
    ClosestHitCollisionCollector<CastShapeCollector> collector;
    query.CastShape(cast, settings, RVec3::sZero(), collector,
                    BroadPhaseLayerFilter(), layerFilter, bodyFilter);
    if (collector.HadHit()) hits.push_back(collector.mHit);
  }

  int32_t n = 0;
  for (const ShapeCastResult& hit : hits) {
    if (n >= capacity) break;
    Vec3 axis = hit.mPenetrationAxis;
    Vec3 normal = axis.LengthSq() > 1.0e-12f ? -axis.Normalized() : Vec3::sZero();
    writeHit(w, out[n++], hit.mBodyID2, hit.mFraction,
             RVec3(hit.mContactPointOn2), normal, hit.mPenetrationDepth);
  }
  return n;
}

int32_t jj_overlap(int64_t world, const JPH_Shape* shapePtr, const float* pose,
                   uint32_t layerMask, int64_t ignoreBody, uint32_t flags,
                   JJ_Hit* out, int32_t capacity) {
  std::lock_guard<std::recursive_mutex> guard(gLock);
  commitStack();
  JjWorld* w = findWorld(world);
  if (w == nullptr || shapePtr == nullptr || capacity <= 0) return 0;
  const Shape* shape = reinterpret_cast<const Shape*>(shapePtr);

  BodyID ignored;
  JjWorld* ignoreOwner = nullptr;
  findBody(ignoreBody, &ignoreOwner, &ignored);
  JjIgnoreBody bodyFilter(ignored);
  JjQueryFilter layerFilter(layerMask, flags | JJ_QUERY_ALL_HITS);
  RMat44 transform = RMat44::sRotationTranslation(quat(pose + 3), rvec3(pose))
                         .PreTranslated(shape->GetCenterOfMass());
  CollideShapeSettings settings;
  AllHitCollisionCollector<CollideShapeCollector> collector;
  w->system->GetNarrowPhaseQuery().CollideShape(
      shape, Vec3::sOne(), transform, settings, RVec3::sZero(), collector,
      BroadPhaseLayerFilter(), layerFilter, bodyFilter);

  // One hit per body: its deepest contact.
  std::unordered_map<uint32, size_t> deepest;
  std::vector<CollideShapeResult> hits;
  for (const CollideShapeResult& hit : collector.mHits) {
    uint32 key = hit.mBodyID2.GetIndexAndSequenceNumber();
    auto it = deepest.find(key);
    if (it == deepest.end()) {
      deepest[key] = hits.size();
      hits.push_back(hit);
    } else if (hit.mPenetrationDepth > hits[it->second].mPenetrationDepth) {
      hits[it->second] = hit;
    }
  }
  int32_t n = 0;
  for (const CollideShapeResult& hit : hits) {
    if (n >= capacity) break;
    Vec3 axis = hit.mPenetrationAxis;
    Vec3 normal = axis.LengthSq() > 1.0e-12f ? -axis.Normalized() : Vec3::sZero();
    writeHit(w, out[n++], hit.mBodyID2, 0, RVec3(hit.mContactPointOn2), normal,
             hit.mPenetrationDepth);
  }
  return n;
}

/* ---- Joints ------------------------------------------------------------- */

int64_t jj_joint_create(int64_t world, int64_t bodyA, int64_t bodyB,
                        const JJ_JointSettings* s) {
  std::lock_guard<std::recursive_mutex> guard(gLock);
  JjWorld* w = findWorld(world);
  if (w == nullptr || s == nullptr) return 0;
  JjWorld* ownerA = nullptr;
  JjWorld* ownerB = nullptr;
  BodyID a;
  BodyID b;
  if (!findBody(bodyA, &ownerA, &a) || ownerA != w) return 0;
  if (bodyB != 0 && (!findBody(bodyB, &ownerB, &b) || ownerB != w)) return 0;
  if (a == b) return 0;

  Ref<TwoBodyConstraintSettings> settings = jointSettings(s);
  if (settings == nullptr) return 0;
  BodyInterface& bodies = w->system->GetBodyInterface();
  // B first: see jointSettings. An invalid B is the world.
  Ref<TwoBodyConstraint> constraint = bodies.CreateConstraint(settings, b, a);
  if (constraint == nullptr) return 0;
  w->system->AddConstraint(constraint);
  bodies.ActivateBody(a);
  if (!b.IsInvalid()) bodies.ActivateBody(b);

  uint32 index = freeIndex(w->joints, w->jointSeq);
  JjJoint* j = new JjJoint();
  j->constraint = constraint;
  j->a = a;
  j->b = b;
  j->kind = s->kind;
  j->seq = w->jointSeq[index];
  w->joints[index] = j;
  if (s->motorState != JJ_MOTOR_OFF) applyMotor(j, s->motorState, s->motorTarget);
  return objectHandle(w->tag(kJointHandle), entryLow(index, j->seq));
}

void jj_joint_destroy(int64_t joint) {
  std::lock_guard<std::recursive_mutex> guard(gLock);
  JjWorld* w = nullptr;
  uint32 index = 0;
  if (findJoint(joint, &w, &index) != nullptr) removeJoint(w, index);
}

bool jj_joint_valid(int64_t joint) {
  std::lock_guard<std::recursive_mutex> guard(gLock);
  JjWorld* w = nullptr;
  return findJoint(joint, &w, nullptr) != nullptr;
}

void jj_joint_set_enabled(int64_t joint, bool enabled) {
  std::lock_guard<std::recursive_mutex> guard(gLock);
  JjWorld* w = nullptr;
  JjJoint* j = findJoint(joint, &w, nullptr);
  if (j == nullptr) return;
  j->constraint->SetEnabled(enabled);
  wakeJoint(w, j);
}

void jj_joint_set_motor(int64_t joint, int32_t state, float target) {
  std::lock_guard<std::recursive_mutex> guard(gLock);
  JjWorld* w = nullptr;
  JjJoint* j = findJoint(joint, &w, nullptr);
  if (j == nullptr) return;
  applyMotor(j, state, target);
  wakeJoint(w, j);
}

float jj_joint_value(int64_t joint) {
  std::lock_guard<std::recursive_mutex> guard(gLock);
  JjWorld* w = nullptr;
  JjJoint* j = findJoint(joint, &w, nullptr);
  if (j == nullptr) return 0;
  if (j->kind == JJ_JOINT_HINGE) {
    return static_cast<HingeConstraint*>(j->constraint.GetPtr())->GetCurrentAngle();
  }
  if (j->kind == JJ_JOINT_SLIDER) {
    return static_cast<SliderConstraint*>(j->constraint.GetPtr())->GetCurrentPosition();
  }
  return 0;
}

/* ---- Characters --------------------------------------------------------- */

int64_t jj_character_create(int64_t world, const JJ_CharacterSettings* s,
                            const float* position) {
  std::lock_guard<std::recursive_mutex> guard(gLock);
  JjWorld* w = findWorld(world);
  if (w == nullptr || s == nullptr) return 0;
  if (s->layer < 0 || s->layer >= JJ_LAYER_COUNT) return 0;
  if (!(s->radius > 0) || s->halfHeight < 0) return 0;

  Vec3 up = vec3(s->up);
  up = up.LengthSq() > 1.0e-12f ? up.Normalized() : Vec3(0, 1, 0);
  // Shift the capsule along "up" so the character's position is its feet.
  RefConst<Shape> capsule = RotatedTranslatedShapeSettings(
                                up * (s->halfHeight + s->radius),
                                Quat::sIdentity(),
                                new CapsuleShape(s->halfHeight, s->radius))
                                .Create()
                                .Get();
  Ref<CharacterVirtualSettings> settings = new CharacterVirtualSettings();
  settings->mShape = capsule;
  settings->mUp = up;
  settings->mMaxSlopeAngle = s->maxSlopeAngle;
  settings->mMass = s->mass > 0 ? s->mass : 70.0f;
  settings->mMaxStrength = s->maxStrength >= 0 ? s->maxStrength : 100.0f;
  if (s->padding > 0) settings->mCharacterPadding = s->padding;
  if (s->predictiveContactDistance > 0) {
    settings->mPredictiveContactDistance = s->predictiveContactDistance;
  }
  if (s->penetrationRecoverySpeed > 0) {
    settings->mPenetrationRecoverySpeed = s->penetrationRecoverySpeed;
  }
  settings->mEnhancedInternalEdgeRemoval = s->enhancedInternalEdgeRemoval;
  // Only contacts in the lower sphere of the capsule support the character.
  settings->mSupportingVolume = Plane(up, -s->radius);
  if (s->innerBody) {
    settings->mInnerBodyShape = capsule;
    settings->mInnerBodyLayer = objectLayer(kCharacter, uint32(s->layer));
  }

  uint32 index = freeIndex(w->characters, w->characterSeq);
  JjCharacter* c = new JjCharacter();
  c->layer = uint32(s->layer);
  c->row = w->collides[s->layer];
  c->seq = w->characterSeq[index];
  c->character = new CharacterVirtual(settings, rvec3(position), Quat::sIdentity(),
                                      s->userData, w->system);
  c->character->SetCharacterVsCharacterCollision(&w->characterVsCharacter);
  w->characterVsCharacter.Add(c->character);
  w->characters[index] = c;
  return objectHandle(w->tag(kCharacterHandle), entryLow(index, c->seq));
}

void jj_character_destroy(int64_t character) {
  std::lock_guard<std::recursive_mutex> guard(gLock);
  JjWorld* w = nullptr;
  uint32 index = 0;
  if (findCharacter(character, &w, &index) != nullptr) removeCharacter(w, index);
}

bool jj_character_valid(int64_t character) {
  std::lock_guard<std::recursive_mutex> guard(gLock);
  JjWorld* w = nullptr;
  return findCharacter(character, &w, nullptr) != nullptr;
}

int64_t jj_character_inner_body(int64_t character) {
  std::lock_guard<std::recursive_mutex> guard(gLock);
  JjWorld* w = nullptr;
  JjCharacter* c = findCharacter(character, &w, nullptr);
  if (c == nullptr) return 0;
  BodyID id = c->character->GetInnerBodyID();
  return id.IsInvalid() ? 0 : w->bodyHandle(id);
}

void jj_character_step(int64_t character, float dt, const float* velocity,
                       const float* gravity, const float* stepDown,
                       const float* stepUp, JJ_CharacterState* out) {
  std::lock_guard<std::recursive_mutex> guard(gLock);
  commitStack();
  JjWorld* w = nullptr;
  JjCharacter* c = findCharacter(character, &w, nullptr);
  if (c == nullptr) {
    std::memset(out, 0, sizeof(JJ_CharacterState));
    out->groundState = JJ_GROUND_IN_AIR;
    return;
  }
  // Characters on layers this one's grid row leaves out pass through it.
  auto& others = w->characterVsCharacter.mCharacters;
  others.clear();
  for (JjCharacter* o : w->characters) {
    if (o != nullptr && (c->row & (1u << o->layer)) != 0 &&
        (o->row & (1u << c->layer)) != 0) {
      others.push_back(o->character);
    }
  }

  CharacterVirtual& ch = *c->character;
  ch.SetLinearVelocity(vec3(velocity));
  CharacterVirtual::ExtendedUpdateSettings update;
  update.mStickToFloorStepDown = vec3(stepDown);
  update.mWalkStairsStepUp = vec3(stepUp);
  JjCharacterFilter layerFilter(c->row);
  ch.ExtendedUpdate(dt, vec3(gravity), update, BroadPhaseLayerFilter(),
                    layerFilter, BodyFilter(), ShapeFilter(), *w->temp);
  writeCharacterState(w, c, out);
}

void jj_character_state(int64_t character, JJ_CharacterState* out) {
  std::lock_guard<std::recursive_mutex> guard(gLock);
  JjWorld* w = nullptr;
  JjCharacter* c = findCharacter(character, &w, nullptr);
  if (c == nullptr) {
    std::memset(out, 0, sizeof(JJ_CharacterState));
    out->groundState = JJ_GROUND_IN_AIR;
    return;
  }
  writeCharacterState(w, c, out);
}

void jj_character_set_position(int64_t character, const float* position) {
  std::lock_guard<std::recursive_mutex> guard(gLock);
  JjWorld* w = nullptr;
  JjCharacter* c = findCharacter(character, &w, nullptr);
  if (c != nullptr) c->character->SetPosition(rvec3(position));
}

int32_t jj_character_contacts(int64_t character, JJ_CharacterContact* out,
                              int32_t capacity) {
  std::lock_guard<std::recursive_mutex> guard(gLock);
  JjWorld* w = nullptr;
  JjCharacter* c = findCharacter(character, &w, nullptr);
  if (c == nullptr || capacity <= 0) return 0;
  int32_t n = 0;
  for (const CharacterContact& k : c->character->GetActiveContacts()) {
    if (n >= capacity) break;
    JJ_CharacterContact& o = out[n++];
    std::memset(&o, 0, sizeof(o));
    o.body = k.mBodyB.IsInvalid() ? 0 : w->bodyHandle(k.mBodyB);
    if (!k.mCharacterIDB.IsInvalid()) {
      for (uint32 i = 0; i < w->characters.size(); i++) {
        JjCharacter* other = w->characters[i];
        if (other != nullptr && other->character->GetID() == k.mCharacterIDB) {
          o.character = objectHandle(w->tag(kCharacterHandle), entryLow(i, other->seq));
          break;
        }
      }
    }
    putR3(o.position, k.mPosition);
    put3(o.normal, k.mContactNormal);
    put3(o.velocity, k.mLinearVelocity);
    o.distance = k.mDistance;
    o.isSensor = k.mIsSensorB;
    o.hadCollision = k.mHadCollision;
  }
  return n;
}

/* ---- Debug lines -------------------------------------------------------- */

int32_t jj_debug_lines(int64_t world, uint32_t flags, float* out, int32_t capacity) {
#ifdef JPH_DEBUG_RENDERER
  std::lock_guard<std::recursive_mutex> guard(gLock);
  JjWorld* w = findWorld(world);
  if (w == nullptr) return 0;
  if (gLines == nullptr) gLines = new JjLineRenderer();
  gLines->lines.clear();
  BodyManager::DrawSettings settings;
  settings.mDrawShape = true;
  settings.mDrawShapeWireframe = true;
  w->system->DrawBodies(settings, gLines);
  if ((flags & 1u) != 0) w->system->DrawConstraints(gLines);
  int32_t total = int32_t(gLines->lines.size() / JJ_DEBUG_LINE_FLOATS);
  int32_t n = total < capacity ? total : capacity;
  if (n > 0 && out != nullptr) {
    std::memcpy(out, gLines->lines.data(), sizeof(float) * JJ_DEBUG_LINE_FLOATS * size_t(n));
  }
  return total;
#else
  (void)world;
  (void)flags;
  (void)out;
  (void)capacity;
  return -1;
#endif
}

}  // extern "C"
