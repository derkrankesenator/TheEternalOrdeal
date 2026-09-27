#pragma once

// Resolved via the consuming project's own include path (-I include), not
// relative paths - this module lives in its own repo (see README.md) and
// only needs to sit alongside a normal WiiXLaunch project's include/, not be
// physically nested inside its include/wiixlaunch/ tree.
#include <wiixlaunch/platform.hpp>
#include <wiixlaunch/hook.hpp>
#include <wiixlaunch/call.hpp>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <vector>
#include <type_traits>

// No WIIXL_LOG used; consuming project knows its logging signature.

// WiiXLaunch::BotW::Actor - a thin wrapper around a raw ksys::act::Actor*,
// plus the actor-name and actor-spawn RE work ported from the "Actor
// Spawning and Weapon Detection" mod's main.cpp / handwritten-symbols-botw.csv.
// Actor spawning is only confirmed on Wii U/Cemu (see SupportsSpawn) - the
// same InstParamPack/spawnActor/requestCreateBaseProc sequence the original
// mod hand-rolled once per attack is here generalized to take any actor
// name/anchor/position instead.

namespace WiiXLaunch::BotW {

class Actor;

// What Actor::Spawn / Actor::SpawnScaled should equip a spawned enemy with.
//
// Every field is optional; a default-constructed SpawnLoadout spawns exactly
// what Spawn always spawned. The strings are copied when Spawn queues the
// request, so they only have to live until Spawn returns.
//
//   weapon  melee weapon - "Weapon_Sword_001", "Weapon_Spear_001",
//           "Weapon_Lsword_001" ... Goes into slot 0 (EquipItem1), the hand.
//   shield  "Weapon_Shield_001" ... Slot 1 (EquipItem2).
//   bow     "Weapon_Bow_001" ... Slot 0 when there is no weapon - the way a
//           bow Bokoblin/Moriblin/Lizalfos is placed in the map - otherwise
//           slot 2 (EquipItem3), the way a Lynel is. A Bokoblin given BOTH
//           holds the sword; its slot 2 is its horn slot, so the bow is
//           attached but no arrow AI uses it. That is how these enemies are
//           built, not a limit of the hook - see WHICH SLOT IS WHAT.
//   arrow   arrow actor for the bow, as a map placement's ArrowName:
//           "NormalArrow", "FireArrow", "IceArrow", "ElectricArrow",
//           "BombArrow_A", "AncientArrow". Answered for the enemy's whole life,
//           not only at init - see ARROWS.
//   slot[]  raw per-slot names, EquipItem<i+1>, overriding the named fields
//           above. For layouts the three named fields do not fit: a quiver
//           ("BokoblinPipe" / "LizalfosPipe" in slot 2, as the map puts next
//           to a bow), a Hinox (EquipItem3..5), or anything else.
//
// Only an Enemy reads any of this; on other actors it has no effect. Wii U /
// Cemu only, like Spawn itself. The game may still upgrade a weapon through
// the level sensor, exactly as it does for a placed enemy.
//
// C++20 designated initialisers make call sites readable:
//   Actor::Spawn("Enemy_Lynel_Junior", anchor, x, y, z,
//                {.weapon = "Weapon_Sword_001", .shield = "Weapon_Shield_001",
//                 .bow = "Weapon_Bow_001", .arrow = "FireArrow"});
struct SpawnLoadout {
    const char* weapon = nullptr;
    const char* shield = nullptr;
    const char* bow = nullptr;
    const char* arrow = nullptr;
    const char* slot[6] = {};
};

namespace impl {

#if !WIIXL_SWITCH

constexpr uint32_t kEnemyEquipSlotCount = 6;   // Enemy::createEquipments loops 0..5

// What one spawn should be equipped with, as copied strings ready to answer
// the two getters (Enemy::getEquippedItem and Enemy::getArrowName - see the
// "Spawning an enemy with equipment" block further down for both). Filled from a SpawnLoadout by BuildEquipRequest, stored in
// PendingSpawn until the flush, then copied into SpawnEquip. Copied rather
// than pointed at for the same reason PendingSpawn::name is.
struct EquipRequest {
    // items[i] answers getEquippedItem(slot i) = EquipItem<i+1>. "" = let the
    // game answer that slot.
    char items[kEnemyEquipSlotCount][64] = {};
    // ArrowName for the bow. "" = the game's own answer.
    char arrow[64] = {};

    bool Any() const {
        if (arrow[0]) return true;
        for (uint32_t i = 0; i < kEnemyEquipSlotCount; ++i)
            if (items[i][0]) return true;
        return false;
    }
    void Clear() {
        for (uint32_t i = 0; i < kEnemyEquipSlotCount; ++i) items[i][0] = '\0';
        arrow[0] = '\0';
    }
};

struct PendingSpawn {
    bool valid = false;
    // The name is COPIED, not pointed at. A request outlives the call by
    // at least a frame - SpawnFlushHook consumes it later - so a caller
    // passing a local buffer (an actor name read off a network request,
    // say) had it go out of scope before the spawn ran. That failed
    // SILENTLY: the request was accepted, the flush read freed stack, and
    // no actor appeared. String literals happened to survive, which made
    // the bug look like "only certain actors can spawn".
    char name[64] = {};
    void* anchor = nullptr;
    float pos[3] = {};
    bool hasScale = false;
    float scale[3] = {1.0f, 1.0f, 1.0f};
    // Optional equipment, built from a SpawnLoadout by BuildEquipRequest and
    // copied for the same reason as name. All-empty means "spawn unarmed",
    // which is what Spawn always did. ExecuteSpawn hands it to ArmSpawnEquip;
    // see the SpawnEquip block for why equipment travels outside the
    // InstParamPack.
    EquipRequest equip;
};

// Copies at most cap-1 bytes and always terminates.
inline void CopyActorName(char* dest, uint32_t cap, const char* src) {
    uint32_t i = 0;
    if (src) {
        for (; i + 1 < cap && src[i]; ++i) dest[i] = src[i];
    }
    dest[i] = '\0';
}

inline PendingSpawn& PendingSpawnRef() {
    static PendingSpawn s;
    return s;
}

// Full InstParamPack: 4-byte header (overwritten by setAnchor with the
// anchor actor) + 196-byte Buffer (2-byte count + 2-byte usedLen +
// 192-byte body) that Buffer::add (FUN_031f9870) writes named params into.
struct InstParamPack {
    void* anchor;
    uint16_t count;
    uint16_t usedLen;
    uint8_t body[192];
};
static_assert(sizeof(InstParamPack) == 200, "must match the real 0xC8-byte InstParamPack layout");

// The 2-word {rawPtr, vtable} "SafeString" shape used all over this binary
// for named-param keys, padded well past the 2 fields the writer itself
// reads directly - a virtual call through the object can write back into it
// while resolving/caching the value.
struct SafeStringRef {
    void* rawPtr;
    void* vtable;
    uint8_t padding[24];
};

using SpawnActorFn = int (*)(void* mgr, const char* name, void* heap, void* handleOut, void* paramPack, int unused, int priority);
using SetParamPackAnchorFn = void (*)(void* paramPack, void* anchorActor);
using CreateBaseProcFn = int (*)(void* initializer, void* request);
using WriteParamFn = void (*)(void* pack, void* value, void* keyRef, int sizeBytes, uint8_t typeTag);
using InitParamPackBufferFn = void* (*)(void* buffer);

constexpr uintptr_t kPositionKeyWord0 = 0x10072ed4;  // DAT_10072ed4, from doSpawn_Conf98's direct FUN_031f9870 call
constexpr uintptr_t kSafeStringVtable = 0x10263910;  // DAT_10263910, the shared resolver vtable reused everywhere

// "@S", the InstParamPack scale key, at 0x1031b4ec - the same kind of two-byte
// named param as "@P" and written exactly the same way: 12 bytes, type tag 4.
//
// Found from the game's own setter, ksys::act::ActorCreator::addScale at
// 0x037b55a4, which takes ONE float, splats it into a stack vec3 and calls
// 0x031f9870(pack+4, &vec, &key, 0xc, 4). That is the uniform-scale overload;
// the decompilation has a second one taking a sead::Vector3f and writing the
// same key, so a NON-UNIFORM scale is representable and is what SpawnScaled
// writes. The key string sits in a run of them - 0x1031b4ef is "@W",
// 0x1031b514 "@DD", 0x1031b518 "@RL", 0x1031b51c "@TV".
constexpr uintptr_t kScaleKeyWord0 = 0x1031b4ec;

constexpr uintptr_t kActorCreateMgrAddr = 0x1047c2b8;   // global: manager pointer, passed straight through
constexpr uintptr_t kHeapProviderAddr = 0x10463f6c;     // global: pointer to a struct; +0x10 field is the spawn heap
constexpr uint32_t kHeapFieldOffset = 0x10;

// --- ksys::act::Actor transform (Wii U V208) ---
//
// Actor+0x1F8 is the actor's sead::Matrix34f - the object the game's own debug
// name for it calls ACTOR_MATRIX. Row-major 3x4, 12 floats, 0x30 bytes, so it
// spans 0x1F8..0x227:
//
//   m[0][0..3]  0x1F8 0x1FC 0x200 0x204
//   m[1][0..3]  0x208 0x20C 0x210 0x214
//   m[2][0..3]  0x218 0x21C 0x220 0x224
//
// Confirmed two ways. 0x0383b398 copies exactly 12 floats out of
// playerActor+0x1F8 via the matrix-assign helper at 0x03c6fe5c, which fixes
// both the base and the size. And the game reads gameplay position out of the
// translation column - m[0][3]/m[1][3]/m[2][3], i.e. +0x204/+0x214/+0x224 -
// in 0x022b1278 (item-drop spawn), 0x022ae534 and 0x025bba8c, which is where
// the otherwise baffling 0x10 stride between "x", "y" and "z" comes from.
//
// The 0x10 stride is also why the old rotation offsets were wrong. 0x1E4 and
// 0x1F4 sit *before* this matrix and 0x234 sits *after* it (the matrix ends at
// 0x227), so they were three unrelated fields, not pitch/yaw/roll. Rotation
// lives in the 3x3 basis above; there is no Euler triple on the actor.
constexpr uint32_t kActorMatrixOffset = 0x1f8;
constexpr uint32_t kActorMatrixFloats = 12;
constexpr uint32_t kPosXOffset = kActorMatrixOffset + 0x0c;  // 0x204
constexpr uint32_t kPosYOffset = kActorMatrixOffset + 0x1c;  // 0x214
constexpr uint32_t kPosZOffset = kActorMatrixOffset + 0x2c;  // 0x224

// Contiguous sead::Vector3f velocity - see Actor::GetLinearVelocity for how
// this was identified and why it is a different kind of field to the position.
constexpr uint32_t kVelocityOffset = 0x25c;

// The character controller's desired-velocity input, relative to the
// controller (actor +0x3a0 -> +0x50). Found by in-process injection sweep on
// a live game; see Actor::GetControllerVelocityPtr for the evidence and for
// the two rules that make writes to it work.
constexpr uint32_t kControllerVelocityOffset = 0xb0;

// The character controller's desired-DIRECTION input - a unit vector, not an
// angular rate. Found statically, not by sweep: tracing the "RotSpeed"
// AIProgram parameter (game::ai action classes parse it alongside
// "RotAccRatio"/"RotRatio") from its parser into a move-toward-target action's
// per-frame update led to a function that computes the direction to the
// target, clamps how far it may turn this frame using that cached RotSpeed,
// and writes the resulting turn-limited unit vector here via a plain 3-float
// store - see Actor::GetControllerDirectionPtr for the call chain.
//
// UNCONFIRMED LIVE as of this writing - the static evidence is solid (the
// turn-rate clamp math sits directly upstream of the store), but nothing has
// yet measured whether writing this field actually turns a live actor, or
// whether the controller's own per-frame step (not yet located) still has to
// run for it to take effect the way +0xb0 does for velocity.
constexpr uint32_t kControllerDirectionOffset = 0x34;

// Sibling of kControllerDirectionOffset: the desired-speed SCALAR the same
// move-toward-target function writes alongside the direction, via a
// different setter. Likely combines with the direction above into the
// velocity vector at kControllerVelocityOffset once per physics tick, though
// the internal step that would do that combining has not been located.
// Recorded here rather than acted on - not yet exposed as an accessor.
constexpr uint32_t kControllerSpeedOffset = 0x30;

// The byte Actor::setVelocity sets after writing a velocity, and the whole
// reason writing +0x25c alone does nothing.
//
// +0x25c is BOTH the per-frame cache the physics writes (0x037966bc) and the
// requested velocity the actor's update consumes. Writing the floats without
// this flag leaves a value that reads back correctly, is seen by whatever
// samples the cache that frame - the camera visibly shifted - and is then
// overwritten. Measured: 121 writes of 25.0 moved the player zero units.
//
// Wii U Actor::setVelocity is 0x03798b60, matched to Switch's symbolised
// Actor::setVelocity through uking::action::StopASIgnite (the class that
// loads "IgniteVelocityDir"), whose m32 calls it in both builds.
constexpr uint32_t kVelocityRequestFlagOffset = 0x432;

// Actor::setVelocity refuses unless this byte is 0 or 2.
constexpr uint32_t kVelocityStateOffset = 0x54;

// setActorMtxOnly(actor, const sead::Matrix34f&, const sead::Vector3f* vel) at
// 0x03798ae8 - the reason writing +0x204/+0x214/+0x224 by hand never moved
// anything. That matrix is the actor's *input* transform; the game reads the
// transform it actually uses from a second matrix at +0x22C, and 0x03798ae8 is
// what keeps the two in step.
//
// NOT ksys::act::Actor::setMtx, despite an earlier comment here saying so. That
// is 0x0379fb54, reached as virtual index 85, and the two are siblings rather
// than caller and callee - this one never calls it. The difference is the whole
// story for anything physics drives: 0x03798ae8 writes the actor side and
// stops, so for an actor with a character controller or a rigid body,
// Actor::updateMtxFromPhysics (virtual index 84) republishes the matrix from
// physics on the very next frame and the write is gone. Use it to place an
// actor physics does not drive; use SetMtx() below for anything else.
//
//   0x03798ae8(actor, mtx, vel):
//     0x03c6fe5c(mtx, actor + 0x1F8)   // copy into ACTOR_MATRIX
//     0x037986e4(actor, mtx)           // publish to actor+0x22C, routed through
//                                      // the attach frame at actor+0x3B8 when
//                                      // the actor is riding/parented
//     if (vel) actor+0x274/0x278/0x27C = vel                 // optional
//
// Poking only the first matrix leaves +0x22C stale, and the actor's own update
// overwrites the first one from the second on the next frame - which is exactly
// the "the field changes and the model doesn't" behaviour already documented on
// Spawn() below. ~30 gameplay call sites use 0x03798ae8, so it is the game's
// normal way to place an actor, not a back door.
using SetActorMtxFn = void (*)(void* actor, const float* mtx34, const float* velocity);
constexpr uintptr_t kSetActorMtxAddr = 0x03798ae8;

// --- ksys::act::Actor::setMtx, the authoritative placement path -------------
//
// virtual void setMtx(const sead::Matrix34f&, bool setActorMtx, bool refresh)
//
// The only function that pushes a transform into every representation at once:
// the actor matrix (+0x1F8), the render model's matrix, and the PHYSICS side -
// preferring the character controller, falling back to the main rigid body -
// then refreshing the physics instance set so cloth and ragdoll do not stretch
// across the move. Every Warp* AI action in the game calls it;
// uking::action::WarpToPos::oneShot_ is the reference, and passes (mtx, 1, 1).
//
// Called virtually rather than by address, because the player OVERRIDES it -
// on this build the base is 0x0379fb54 and Link's override is 0x02d66abc, which
// additionally updates his facing from the matrix basis and his own position
// copies. Going through the vtable gets the right one for whatever actor it is.
//
//   setActorMtx  true  write the actor and model matrices now. false stages
//                      into the +0x228 mirror and sets ActorFlag bit 2, which
//                      updateMtxFromPhysics only ever picks up for an actor
//                      with NO body at all - so false is not what you want for
//                      anything physics drives.
//   refresh      true  reset the physics instance set afterwards (cloth,
//                      ragdoll). Both of the game's warp actions pass true.
//
// Confirmed against a running game: index 85 resolved to Link's override, and
// the two slots this file already pinned by byte offset (getMaxLife, getLife)
// land on real functions under the same arithmetic.
constexpr uint32_t kSetMtxVtableSlot = 0x2ac;   // virtual index 85
using ActorSetMtxFn = void (*)(void* actor, const float* mtx34, int setActorMtx, int refresh);

// --- ksys::act::BaseProcMgr job lists (Wii U V208) ---
// Container geometry for Actor::ForEachDynamic; the derivation and the
// functions it was read from are documented there.
constexpr uintptr_t kBaseProcMgrAddr = 0x1047c244;  // global: BaseProcMgr*
constexpr uint32_t kJobBucketSize = 0xc0;           // one job type: 8 slots x 0x18
constexpr uint32_t kProcListSize = 0x0c;            // {prev, next, s32 count}
constexpr uint32_t kListsPerBucket = kJobBucketSize / kProcListSize;  // 16
constexpr uint32_t kMaxJobTypes = 32;               // sanity bound, not a game constant

// Pointer set used to visit each proc once when it is registered under more
// than one job type. Open addressed, never grows, cleared per traversal;
// a full table degrades to allowing duplicates rather than dropping actors.
struct ProcVisitSet {
    static constexpr size_t kSlots = 2048;  // power of two
    void* slots[kSlots];

    void Clear() {
        for (size_t i = 0; i < kSlots; ++i) slots[i] = nullptr;
    }

    // True if p had not been seen yet.
    bool Add(void* p) {
        size_t i = (reinterpret_cast<uintptr_t>(p) >> 4) & (kSlots - 1);
        for (size_t probe = 0; probe < kSlots; ++probe) {
            if (slots[i] == nullptr) {
                slots[i] = p;
                return true;
            }
            if (slots[i] == p) return false;
            i = (i + 1) & (kSlots - 1);
        }
        return true;
    }
};

inline ProcVisitSet& ProcVisitSetRef() {
    static ProcVisitSet s = {};
    return s;
}

// True while a traversal is in progress, so a nested one does not wipe the
// outer one's visit set out from under it.
inline bool& InProcTraversal() {
    static bool v = false;
    return v;
}

// BaseProcMgr's own lock, at mgr+0x80. Held while walking the job lists -
// BotW runs actor jobs on three threads (BaseProcMgr+0x11C/0x120/0x124, see
// the thread check at 0x0378f210), so the lists mutate under an unlocked
// reader and a stale `next` walks straight into freed memory.
//
// sead::CriticalSection keeps its OSMutex at +0x10 and these two are thin
// wrappers over it - 0x030bb668 is `r3 += 0x10; b OSLockMutex` and 0x030bb69c
// is `r3 += 0x10; b OSUnlockMutex`. Cafe OS mutexes are recursive, so taking
// this on a thread that already holds it is safe; the game's own deleteLater
// (0x0378a374) locks the same field the same way.
constexpr uint32_t kBaseProcMgrLockOffset = 0x80;
using CriticalSectionFn = void (*)(void* cs);
constexpr uintptr_t kCriticalSectionLockAddr = 0x030bb668;
constexpr uintptr_t kCriticalSectionUnlockAddr = 0x030bb69c;

class ProcMgrLock {
public:
    explicit ProcMgrLock(void* cs) : m_Cs(cs) {
        if (m_Cs) WiiXLaunch::GetTargetFunction<CriticalSectionFn>(0x0, kCriticalSectionLockAddr)(m_Cs);
    }
    ~ProcMgrLock() {
        if (m_Cs) WiiXLaunch::GetTargetFunction<CriticalSectionFn>(0x0, kCriticalSectionUnlockAddr)(m_Cs);
    }
    ProcMgrLock(const ProcMgrLock&) = delete;
    ProcMgrLock& operator=(const ProcMgrLock&) = delete;

private:
    void* m_Cs;
};

// The creation-request tracker handed to spawnActor. Not the finished actor,
// and not the pooled unit either - it stays small.
//
// Read at 0x0378a70c (V208 Wii U), which takes it as `handle` alongside the
// new proc:
//
//   +0x00  int   request state; the create path only runs when this is 1
//   +0x04  byte  "already resolved" flag
//
// Nothing has been observed reading past +0x08, and 0x0378a70c is the only
// caller of BaseProcHandle::setProc, so 16 bytes is generous rather than tight.
//
// Word 0 was previously described here as a pool-slot pointer. It is not: it is
// the request-state int above, which is why the "set slot+4" write that used to
// follow the forced createBaseProc call below never did anything.
//
// The handle never receives the created actor. That lands on the pooled
// BaseProcUnit instead - 0x25C bytes from the 256-entry pool at 0x105597c0
// (see the pool set-up at 0x0378d22c) - whose own layout is:
//
//   +0x00  state (0..5)          +0x08  BaseProc*
//   +0x04  link; 0x105597b8 is the "detached" sentinel
//   +0x220 sead::CriticalSection
//
// and a proc points back at its unit through BaseProc+0xE0. Getting a spawned
// actor back means going through one of those, not through this handle - see
// the note on Actor::Spawn.
inline uint8_t* SpawnedHandle() {
    alignas(8) static uint8_t handle[16] = {};
    return handle;
}

// Gates RequestCreateBaseProcFixHook's forced createBaseProc call so it only
// fires for spawns WE trigger, never for the game's own real spawns.
inline bool& InOurSpawnCall() {
    static bool v = false;
    return v;
}

// Guards RequestCreateBaseProcFixHook's forced createBaseProc call - that
// hook fires 3x per single spawnActor call for reasons not yet understood,
// and without this it would construct 3 actors per spawn instead of 1.
inline bool& ForcedCreateBaseProcThisSpawn() {
    static bool v = false;
    return v;
}

// --- Spawning an enemy with equipment (Wii U V208) -------------------------
//
// Covers Actor::Spawn(..., weapon) and Actor::Spawn(..., SpawnLoadout): melee
// weapon, shield, bow and arrow type. The weapons go through getEquippedItem
// (this block); the arrow type goes through a second getter, see ARROWS
// further down.
//
// THE PROBLEM. A Spawn()ed enemy comes up unarmed, while the same enemy
// placed in a map holds its weapon. The weapon is not a separate actor the
// map places next to the enemy. The enemy creates it itself during init, from
// a name that only a map placement provides.
//
// HOW AN ENEMY ARMS ITSELF. Enemy::createEquipments (0x0201fc34) runs once
// during the enemy's init and does, per weapon slot 0..5:
//
//   name = this->getEquippedItem(slot);          // vtable +0x57c
//   if (!name) continue;
//   (optionally rewrite name via 0x032afe0c)     // see "level sensor" below
//   Buffer::init   0x031f9818(pack)
//   "@M"   = this matrix (actor+0x1f8, 0x30 bytes, type 7) via 0x031f9870
//   "@S"   = this scale  (actor+0x274)               via 0x037b55a4
//   "Life" = -1, "IsPlayerPut" = false, "SharpWeaponJudgeType" = modifier
//   weapon = 0x037b5cb8(*(0x1047c2b8), name, *(*(0x10463f6c) + 0x10), &pack, 1, 0)
//   on failure: deleteLater 0x0378a374(weapon, 0) and return 0
//   BaseProcLink::acquire 0x0378dce8(&this[+0x83c][slot], weapon)  // 0xc stride
//   this[+0xa09] |= 1 << slot                                      // "slot created"
//   this->equipWeapon(slot, weapon, 0, 0)        // vtable +0x524
//     (or 0x02018ecc instead, for an actor carrying tag 0x81c0dcf5)
//
// The actor-create manager at 0x1047c2b8 and the heap at *(0x10463f6c)+0x10
// are the same two globals ExecuteSpawn uses above. That is independent
// confirmation that ExecuteSpawn picked the game's normal creation path.
//
// WHERE THE NAME COMES FROM. Enemy::getEquippedItem (0x0201fa8c) formats
// "EquipItem%d" with slot + 1 and asks the root AI (actor+0x390) for that
// map-unit param through 0x0315b818. It returns nullptr when the param is
// missing or reads "Default". A map-unit param is read from the actor's map
// object (the mubin placement). A Spawn()ed actor has none, so the root AI
// falls back to the AIProgram default, every slot comes back null, and the
// loop above creates nothing. That is the whole reason a spawned Bokoblin is
// unarmed.
//
// WHY NOT THROUGH THE PACK. The pack ExecuteSpawn builds cannot carry this.
// InstParamPack knows "@P", "@R", "@M", "@S", "@D", "@DD", "@TV", "@RV",
// "@MU", "@RL", "@W", "@PC", "@ND", "@DC", "@SB" and "@MA" (all in
// ksys::act::InstParamPack::Buffer in the decompilation). None of them is
// equipment, and getEquippedItem never reads the pack.
//
// THE FIX. EnemyGetEquippedItemHook answers getEquippedItem for the enemy we
// spawned and passes every other call through. Everything after that - the
// weapon actor, the slot link, the +0xa09 bit, equipWeapon, the hold flag the
// AI's WeaponIdx param reads - is the game's own code running as it does for
// a placed enemy. Nothing is written into the enemy by hand. The arrow type
// works the same way through a second getter (EnemyGetArrowNameHook, see
// ARROWS). Hooking the
// getter rather than createEquipments keeps the patch to one answer, and it
// also covers the one override found: 0x02482838 (an Enemy subclass that
// handles tag 0x12ad8456 itself, otherwise tail-calls createEquipments) runs
// its own slot loop through the same vtable +0x57c.
//
// HOW THE ADDRESSES WERE FOUND. Everything was derived in the Switch 1.5.0
// build first, where the decompilation names more, and then located in V208:
//
//   what                          Wii U V208    Switch 1.5.0
//   Enemy::createEquipments       0x0201fc34    0x7100015054 (*)
//   Enemy::getEquippedItem        0x0201fa8c    0x7100014f04
//     virtual index 175           vtable +0x57c vtable +0x578
//   Actor::getWeapons  idx 98     vtable +0x314 vtable +0x310
//   equipWeapon        idx 164    vtable +0x524 vtable +0x520
//   root AI pointer               actor+0x390   actor+0x558
//   RootAi::getMapUnitParam(str)  0x0315b818    0x7100d66508
//   "slot created" bitmask (u8)   actor+0xa09   actor+0xe81
//   equipment flags (u16)         actor+0xa0a   actor+0xe82
//   weapon-name rewrite           0x032afe0c    0x7100e426d4
//   Enemy::isWeaponAlreadyDropped 0x02b6ba28    0x7100731dac
//   Enemy::init2 (calls it)       -             0x7100014688
//
//   (*) uking_functions.csv calls it Enemy::weaponDroppedByEnemy. The body
//       says otherwise: it creates and equips, and it is the only caller of
//       vtable +0x578 in the whole Switch binary.
//
// The Wii U getter was first suggested by function_matches.txt, a heuristic
// Switch<->Wii U pairing (string-literals, score 0.55, on "EquipItem%d" and
// "Default"). It was then confirmed by hand, together with createEquipments,
// which the file has no pair for:
//   * the same body: format "EquipItem%d" with idx + 1, reject "Default",
//     then the same two-bit test on the u16 flags (bit 7, then bit 8 and
//     idx == 0);
//   * the same Enemy_Guardian_Mini special case and 0..5 loop in
//     createEquipments;
//   * the same virtual INDEX for all three calls. A Wii U vtable entry is 8
//     bytes with the function at +4 (see the Life accessors further down), so
//     index N sits at N*8 + 4. Switch entries are plain 8-byte pointers at
//     N*8. 175, 98 and 164 come out identical on both platforms.
// The same file's pairing of ActorWeapons::getEquippedWeapon (0x7100efbc44 ->
// 0x033bccd8, score 0.46, 2 vs 14 parameters) is wrong and was not used.
//
// For reference, what equipWeapon fills in, as read in Switch 1.5.0 only (the
// Wii U layout was not traced): PlayerOrEnemy::getWeapons returns this+0xb90,
// an ActorWeapons with 6 slots of 0x18 bytes starting at +0x08, each
// {BaseProcLink weapon; bool sheathed @+0x10}, owner at +0x98. This is what
// the AI's WeaponIdx param indexes.
//
// MATCHING OUR ENEMY. Spawn() cannot hand the created actor back (see
// Actor::Spawn), so there is no pointer to compare against. The request is
// keyed by actor NAME instead. The first actor of that name to call the getter
// claims it by pointer, answers for all its slots, and the request ends once
// the claimant has asked for its last slot (5). The name is read from
// *(char**)(actor+0x04), the same field createEquipments compares against
// "Enemy_Guardian_Mini" and Actor::GetName reads on Wii U.
//
// UNCONFIRMED LIVE - nothing here has run in game yet. Statically:
//   * createEquipments skips its whole loop when 0x02b6ba28(actor) returns
//     true and bit 8 of actor+0xa0a is clear. 0x02b6ba28 is
//     Enemy::isWeaponAlreadyDropped (Switch 0x7100731dac, named in
//     uking_functions.csv): it needs the actor's map placement (actor+0x7c8
//     on Switch) and checks the "<map>_WeaponDrop_<id>" save flag, i.e. "the
//     player already took this placed enemy's weapon". A spawned enemy has no
//     placement, so it returns false and the loop always runs. Bit 8 is set
//     by Enemy::init2 (Switch 0x7100014688) from the "RideHorseName" map param
//     and only matters for riders. Neither blocks a spawn.
//   * Enemy::init2 calls createEquipments unconditionally, through
//     PlayerOrEnemy::m4 (Switch 0x7100006ffc, vtable +0x570). Only GiantEnemy
//     (Hinox, 0x710002b580) and Sandworm (Molduga, 0x71002cdff8) override
//     +0x570; every other Enemy vtable in the Switch build - Enemy, Dragon,
//     GelEnemy, Guardian, LastBoss, SiteBoss, Swarm - keeps the base one and
//     the base getEquippedItem. There is no Lynel class; a Lynel is an Enemy.
//   * Unless bit 7 of actor+0xa0a is set, the name goes through 0x032afe0c
//     before the actor is created. That is most likely the level sensor's
//     weapon scaling: it takes the name and a modifier and hands back a
//     replacement for both. A requested weapon can therefore come out as an
//     upgraded variant, exactly as it does for a placed enemy.
//   * A weapon in a slot the enemy's AI does not use for it (a bow in a
//     humanoid's slot 2, a shield in slot 0) is created and attached like any
//     other, but no AI picks it up from there - see WHICH SLOT IS WHAT. The
//     slot layout is per enemy family; only Bokoblin, Moriblin, Lizalfos and
//     Lynel were read out, anything else should be checked in its actor pack
//     (Grab) and AIProgram (WeaponIdx) before relying on the named fields.
//   * The arrow override was not measured in game either. It is answered
//     from every getArrowName call for that enemy, but which of the callers
//     listed under ARROWS actually decides the arrow actor a bow shoots was
//     not traced; if the arrow does not change, that is where to look.
//   * A game-triggered spawn of the same actor name in the window between our
//     spawn and our enemy's init can claim the request first. Accepted, for
//     want of anything better to key on.
//   * The getter runs on the thread initialising the enemy, and the request
//     is written from SpawnFlushHook's thread. There is no lock: ArmSpawnEquip
//     clears `valid` first and sets it last, so a reader sees either no
//     request or a complete one. The fields are volatile only to stop the
//     compiler caching them across the two threads.
//
// WHICH SLOT IS WHAT. createEquipments puts EquipItem<n> into weapon slot n-1
// and never re-sorts: Enemy::equipWeapon (vtable +0x524; Switch +0x520 =
// 0x7100016050, "Enemy::m164" in uking_functions.csv) passes the slot straight
// on to ActorWeapons::equipWeapon. So the slot is exactly the one the name was
// returned for. What a slot MEANS is decided by the enemy's own data, in two
// places:
//
//   * GeneralParamList object "Grab" - Slot0Node .. Slot5Node is the bone a
//     slot's weapon is held on, Slot0PodNode .. Slot5PodNode the bone it is
//     stowed on. equipWeapon reads both per slot when it attaches the weapon
//     (0x7100edd258 / 0x7100edd26c on Switch, GParamList object 0x17 = Grab,
//     field +0x140 + slot*0x28).
//   * the AIProgram - every AI/action that touches a weapon names its slot
//     through its WeaponIdx static param (see the ActorWeapons layout above).
//
// Read out of real actor packs and AIPrograms (Relics of the Past 3.1.2 for
// Cemu - a mod, so the numbers below are that mod's data, but these actors
// and AIPrograms are the stock ones as far as could be told):
//
//   Enemy_Bokoblin_Junior / Moriblin_Junior / Lizalfos_Junior
//     Grab:  Slot0Node Weapon_R, Slot1Node Weapon_L,
//            Slot2Node Weapon_L + Slot2PodNode Pipe
//     AI:    WeaponIdx 0 - Attack, CloseSmallAttack, ForkWeaponAttack ... AND
//                          BokoblinArrowAttack, EnemyBaseArrowAttack
//            WeaponIdx 1 - EquipConditionSelect, ScrapEquip, Catch
//            WeaponIdx 2 - HornUse, WeaponOnetimeUse, WeaponDrawn, WeaponHold
//   Enemy_Lynel_Dark (AIProgram Lynel_Senior)
//     AI:    WeaponIdx 0 - LynelBattle, LynelRepeatAttack, IAIAttack ...
//            WeaponIdx 1 - ForkDrawWeaponAtEnter, ForkHoldWeapon
//            WeaponIdx 2 - LynelArrowBattle, ForkASTrgShootArrowWithBaseBone,
//                          EnemySkyArrowAttack
//
// So the three humanoid races use ONE main slot for both melee and bow: slot
// 0 is "the weapon in hand", slot 1 the shield, slot 2 a horn / one-shot item.
// A Lynel has a separate bow slot, 2. The map placements agree (Relics romfs,
// all MainField/AocField/MainFieldDungeon units, counted per shape):
//
//   EquipItem1 Sword/Spear/Lsword                       - most common shape
//   EquipItem1 Sword + EquipItem2 Shield                - 362 + 73
//   EquipItem1 Bow   (+ EquipItem3 BokoblinPipe or LizalfosPipe, the quiver)
//   Lynel: EquipItem1 melee, EquipItem2 Shield, EquipItem3 Bow
//   Hinox ("Giant"): EquipItem3..5 - a different layout again
//
// That is why SpawnLoadout places the bow where it does (see BuildEquipRequest):
// with no melee weapon the bow goes into slot 0, which is what a bow Bokoblin
// in the map has; with a melee weapon too it goes into slot 2, which is right
// for a Lynel. A Bokoblin given both keeps the sword in its hand and gets the
// bow into its horn slot, where it is created and attached but no arrow AI
// looks for it. That is the enemy's design, not something the hook can
// change. SpawnLoadout::slot[] overrides all of this per slot for any other
// layout (a Hinox, or a Pipe quiver in slot 2).
//
// ARROWS. The arrow type is not equipment and is not created by
// createEquipments. It is a separate map-unit param, "ArrowName", read by its
// own virtual getter:
//
//   bool Enemy::getArrowName(Enemy* this, sead::BufferedSafeString* out)
//   Wii U 0x0201901c, vtable +0x52c     Switch 0x7100016368, vtable +0x528
//   (virtual index 165 on both - (0x52c - 4) / 8 and 0x528 / 8; the Switch
//   one is "Enemy::m165" in uking_functions.csv)
//
// It asks the root AI (actor+0x390) for "ArrowName" through the same
// 0x0315b818, copies the value into `out` and returns true, or returns false
// when the param does not exist. The Wii U BufferedSafeString it writes is
// {char* buffer; vtable; s32 bufferSize} - the body writes through out[0] and
// clamps to out[2] - which is the same reversed layout PouchItem::mName has
// in symbols-wiiu-v208.csv.
//
// Unlike getEquippedItem it is NOT only an init-time call. The V208 callers
// found through vtable +0x52c include the enemy's own arrow-stock setup
// (0x02019140, which feeds the name to the actor-info lookup 0x0310da28 on
// the actor-info singleton 0x1046ca84), a generic "arrow name of this actor"
// helper that forwards to the getter for anything that is not the player
// (0x024a1274), and arrow-count queries (0x024a17ac, 0x024a73f8, 0x02190990).
// So the answer has to hold for the enemy's whole life, not just its init.
// That is what ArrowOverrideTable below is for: when an enemy claims a
// request carrying an arrow, its pointer and name go into a small table that
// EnemyGetArrowNameHook answers from for as long as the entry lives.
//
// Values seen for ArrowName in the placements, by count: NormalArrow 3529,
// ElectricArrow 235, FireArrow 166, IceArrow 130, BombArrow_A 118,
// AncientArrow 45. These are arrow ACTOR names, which is what the actor-info
// lookup expects.
//
// function_matches.txt pairs 0x7100016368 with 0x02811d98 (string-literals,
// score 0.55, on "ArrowName" alone). That is wrong - 0x02811d98 is an AI
// action handling "TargetPos" - and was not used. 0x0201901c was found by
// reading the functions that load "ArrowName" through actor+0x390; it is the
// only one that copies into a caller-supplied string and returns a bool, and
// 0x02019140 right behind it calls it through +0x52c.
constexpr uintptr_t kEnemyGetEquippedItemAddr = 0x0201fa8c;
constexpr uintptr_t kEnemyGetArrowNameAddr = 0x0201901c;

// The slots SpawnLoadout's named fields go to - see WHICH SLOT IS WHAT.
constexpr uint32_t kSlotMainHand = 0;   // EquipItem1: melee weapon, or a bow on its own
constexpr uint32_t kSlotShield   = 1;   // EquipItem2
constexpr uint32_t kSlotSecond   = 2;   // EquipItem3: a Lynel's bow; horn/pipe slot on humanoids

// SpawnLoadout -> EquipRequest, applying the slot rules above:
//   weapon       -> slot 0
//   shield       -> slot 1
//   bow          -> slot 0 when there is no weapon, otherwise slot 2
//   arrow        -> ArrowName
//   slot[i]      -> slot i, overriding whatever the named fields put there
inline void BuildEquipRequest(EquipRequest& out, const SpawnLoadout& in) {
    out.Clear();
    auto set = [&](uint32_t slot, const char* name) {
        if (name && name[0]) CopyActorName(out.items[slot], sizeof(out.items[slot]), name);
    };
    set(kSlotMainHand, in.weapon);
    set(kSlotShield, in.shield);
    if (in.bow && in.bow[0]) set((in.weapon && in.weapon[0]) ? kSlotSecond : kSlotMainHand, in.bow);
    for (uint32_t i = 0; i < kEnemyEquipSlotCount; ++i) set(i, in.slot[i]);
    if (in.arrow && in.arrow[0]) CopyActorName(out.arrow, sizeof(out.arrow), in.arrow);
}

// The one armed request. There is one because PendingSpawn holds one spawn,
// and because without a proc pointer only the name tells two requests apart.
struct SpawnEquip {
    // A request is armed and not yet fully answered.
    volatile bool valid = false;
    // Actor name the request is for, compared against *(char**)(actor+0x04).
    char actorName[64] = {};
    // What to answer, per slot, plus the arrow handed on to ArrowOverrideTable.
    EquipRequest req;
    // The enemy that claimed the request, so its later slots are answered even
    // if another actor of the same name starts asking in between.
    void* volatile claimedBy = nullptr;
};

inline SpawnEquip& SpawnEquipRef() {
    static SpawnEquip s;
    return s;
}

// Enemies whose ArrowName we answer, for their whole life - see ARROWS.
//
// Filled when an enemy claims a SpawnEquip request that carries an arrow.
// Entries are never removed: an enemy's death is not observed here. Instead
// each entry is checked on every use against both the pointer AND the name at
// actor+0x04, so memory reused by a different kind of actor does not inherit
// the arrow. Memory reused by another actor of the SAME name would - accepted,
// the same limit the name match already has. 32 entries, oldest overwritten
// first, which only matters with more than 32 arrow-armed spawns alive.
//
// Written from the thread initialising an enemy, read from the AI threads.
// An entry is published by writing `enemy` last and retired by clearing it
// first, so a reader never sees a pointer paired with half-written strings.
struct ArrowOverride {
    void* volatile enemy = nullptr;
    char actorName[64] = {};
    char arrow[64] = {};
};

struct ArrowOverrideTable {
    static constexpr uint32_t kEntries = 32;
    ArrowOverride e[kEntries];
    uint32_t next = 0;
};

inline ArrowOverrideTable& ArrowOverridesRef() {
    static ArrowOverrideTable t;
    return t;
}

inline void AddArrowOverride(void* enemy, const char* actorName, const char* arrow) {
    ArrowOverrideTable& t = ArrowOverridesRef();
    // Same enemy asking again (a second createEquipments pass) reuses its entry.
    uint32_t idx = t.next;
    for (uint32_t i = 0; i < ArrowOverrideTable::kEntries; ++i) {
        if (t.e[i].enemy == enemy) { idx = i; break; }
    }
    if (idx == t.next) t.next = (t.next + 1) % ArrowOverrideTable::kEntries;
    ArrowOverride& o = t.e[idx];
    o.enemy = nullptr;
    CopyActorName(o.actorName, sizeof(o.actorName), actorName);
    CopyActorName(o.arrow, sizeof(o.arrow), arrow);
    o.enemy = enemy;
}

// The Wii U actor name, *(char**)(actor+0x04), or nullptr when that word is
// not a MEM2 pointer. createEquipments reads the name this way for its
// Enemy_Guardian_Mini check. The range is the Wii U band Actor::IsReadablePtr
// uses, which is private to Actor and so not reachable from here.
inline const char* RawActorName(void* actor) {
    const char* name = *reinterpret_cast<const char* const*>(
        static_cast<const uint8_t*>(actor) + 0x04);
    const uintptr_t v = reinterpret_cast<uintptr_t>(name);
    return (v >= 0x10000000 && v < 0xa0000000) ? name : nullptr;
}

// Arms the request for the actor about to be spawned.
//
// Called from ExecuteSpawn BEFORE spawnActor, not after. The enemy may build
// its equipment inside that very call, since RequestCreateBaseProcFixHook
// runs createBaseProc synchronously for our spawns. Whether init actually
// reaches createEquipments inside that call or on a later frame was not
// measured; arming first is correct either way.
//
// A spawn WITHOUT equipment leaves an earlier armed request alone rather than
// clearing it, because the enemy that request belongs to may still be
// initialising. A new ARMED spawn replaces it. Callers queueing several armed
// enemies wait on Actor::IsWeaponRequestPending between them.
//
// An arrow-only request is armed too: the arrow reaches ArrowOverrideTable
// through the claim in EnemyGetEquippedItemHook, which every enemy passes
// through during init whether or not it gets a weapon.
// Counters for finding out where an equipped spawn goes wrong, read through
// Actor::GetEquipDebug(). Nothing in the path above can report an error back
// to the caller - the getter is called by the game, from the enemy's init,
// often on another thread - so these are the only way to tell "the hook never
// ran", "it ran but never matched our enemy" and "it answered, and the game
// still did not create the weapon" apart in a live session.
//
// Plain counters, written without a lock from whichever thread runs the hook.
// A torn read shows up as one off-by-one frame in a log and nothing worse.
struct EquipDebug {
    volatile uint32_t arms = 0;           // ArmSpawnEquip calls that armed a request
    volatile uint32_t getterCalls = 0;    // every getEquippedItem call, any actor
    volatile uint32_t getterWhileArmed = 0; // ...of those, while a request was armed
    volatile uint32_t claims = 0;         // requests claimed by a matching enemy
    volatile uint32_t slotsAnswered = 0;  // slots answered with one of our names
    volatile uint32_t arrowCalls = 0;     // every getArrowName call, any actor
    volatile uint32_t arrowsAnswered = 0; // ...of those answered from ArrowOverrideTable
    // Name of the last actor that asked while a request was armed, whether it
    // matched or not. A mismatch between this and the requested name is the
    // first thing to look for.
    char lastAskedWhileArmed[64] = {};
    int32_t lastSlotAnswered = -1;
};

inline EquipDebug& EquipDebugRef() {
    static EquipDebug d;
    return d;
}

inline void ArmSpawnEquip(const char* actorName, const EquipRequest* req) {
    if (!req || !req->Any()) return;
    EquipDebugRef().arms = EquipDebugRef().arms + 1;
    SpawnEquip& e = SpawnEquipRef();
    e.valid = false;
    e.claimedBy = nullptr;
    CopyActorName(e.actorName, sizeof(e.actorName), actorName);
    e.req = *req;
    e.valid = true;
}

inline void ExecuteSpawn(const char* actorName, void* anchor, float x, float y, float z,
                         const float* scale, const EquipRequest* equip = nullptr) {
    ForcedCreateBaseProcThisSpawn() = false;

    void* mgr = *reinterpret_cast<void**>(kActorCreateMgrAddr);
    if (!mgr) return;

    void* heapOwner = *reinterpret_cast<void**>(kHeapProviderAddr);
    if (!heapOwner) return;
    void* heap = *reinterpret_cast<void**>(static_cast<uint8_t*>(heapOwner) + kHeapFieldOffset);

    // One static pack, reused every spawn - so it has to be reset every spawn.
    //
    // InstParamPack::Buffer::init (0x031f9818) zeroes the param count, the used
    // length and the 192-byte body, and the game calls it before filling a pack
    // in. This does not, which is harmless on the first spawn only: after that
    // the count and used length carry over, so each call appends another copy
    // of the position parameter to a buffer that is never cleared.
    InstParamPack pack = {};
    auto initPackBuffer = WiiXLaunch::GetTargetFunction<InitParamPackBufferFn>(0x0, 0x031f9818);
    initPackBuffer(&pack.count);
    pack.anchor = nullptr;

    auto setAnchor = WiiXLaunch::GetTargetFunction<SetParamPackAnchorFn>(0x0, 0x037b55fc);
    setAnchor(&pack, anchor);

    // "@P" position, or the actor spawns at the pack's zeroed-out default
    // instead of near the anchor.
    auto writeParam = WiiXLaunch::GetTargetFunction<WriteParamFn>(0x0, 0x031f9870);
    alignas(8) static SafeStringRef positionKey = { reinterpret_cast<void*>(kPositionKeyWord0), reinterpret_cast<void*>(kSafeStringVtable) };
    float position[3] = { x, y, z };
    writeParam(&pack.count, position, &positionKey, 0xc, 4);

    // "@S", only when asked for. Left out entirely otherwise, so an ordinary
    // Spawn() writes exactly the pack it always did.
    if (scale) {
        alignas(8) static SafeStringRef scaleKey = { reinterpret_cast<void*>(kScaleKeyWord0), reinterpret_cast<void*>(kSafeStringVtable) };
        float scaleValue[3] = { scale[0], scale[1], scale[2] };
        writeParam(&pack.count, scaleValue, &scaleKey, 0xc, 4);
    }

    // Before spawnActor, not after - see ArmSpawnEquip. A no-op when equip is
    // null or empty.
    ArmSpawnEquip(actorName, equip);

    auto spawnActor = WiiXLaunch::GetTargetFunction<SpawnActorFn>(0x0, 0x037b5e8c);
    InOurSpawnCall() = true;
    spawnActor(mgr, actorName, heap, SpawnedHandle(), &pack, 0, 1);
    InOurSpawnCall() = false;
}

// ksys::act::BaseProcInitializer::requestCreateBaseProc_Conf67 (0x03948cb8)
// always reaches state=2 by submitting our task to a queue that never
// actually drains it for spawns we trigger (real spawns work fine through
// the identical code path - the bug is specific to calling it outside the
// game's own spawn flow). Fix: call createBaseProc ourselves, synchronously,
// with the same real request object - only for our own spawn calls, never
// touching real game-triggered spawns.
WIIXL_HOOK_DEFINE_TRAMPOLINE(RequestCreateBaseProcFixHook) {
    static int Callback(void* initializer, void* request) {
        int result = Orig(initializer, request);

        if (!InOurSpawnCall() || ForcedCreateBaseProcThisSpawn()) return result;
        ForcedCreateBaseProcThisSpawn() = true;

        auto createBaseProc = WiiXLaunch::GetTargetFunction<CreateBaseProcFn>(0x0, 0x03948ed8);
        createBaseProc(initializer, request);

        // A "set slot+4 for pool return on release" write used to sit here,
        // reading handle word 0 as a pool-slot pointer. Word 0 is the request
        // state (see SpawnedHandle), so the value was only ever 0, 1 or 2: the
        // write was dead whenever the state was 0, and a store to address 0x5
        // or 0x6 for any other value. Removed rather than repaired - the unit
        // it was reaching for is pooled and released by the game itself.

        return result;
    }
};

// FUN_024adac8 - an actor per-frame update. Every real spawn is issued from
// inside some actor's own calc/update, never from a generic system-wide
// dispatcher, so a pending Spawn() request is flushed from here too, matching
// that pattern.
//
// This was previously described as "Player-specific". It is not. Logging its
// param_1 across many frames showed it arriving as Weapon_Sword_044 and as
// several other actors, on more than one of the actor job threads. So it fires
// many times per frame for different actors, and its argument is whichever
// actor is currently ticking - never assume it is the player. Anything that
// wants Link must fetch him through Player::GetRaw().
//
// Coverage is confirmed for weapons and physics props (they tick through here
// reliably, every session tested) but NOT confirmed for AI-driven actors: a
// live Enemy_Bokoblin_Junior, targeted via Actor::OnUpdate (which is this
// hook's callback), never once matched in over three seconds/180 frames in
// the same session where weapons ticked through it normally. Whether enemies
// route through a different per-frame update entirely, only reach this one
// under specific conditions (combat, movement, a particular job type), or
// that one actor just happened not to tick during the window measured, is
// still open. Do not assume Actor::OnUpdate sees every actor kind until an
// enemy is confirmed here directly.
WIIXL_HOOK_DEFINE_TRAMPOLINE(SpawnFlushHook) {
    using RawCallbackFn = void (*)(void* playerPtr);
    static RawCallbackFn& CallbackRef() { static RawCallbackFn fn = nullptr; return fn; }

    static void Callback(void* param1, void* param2) {
        Orig(param1, param2);

        if (param1) {
            PendingSpawn& pending = PendingSpawnRef();
            if (pending.valid) {
                pending.valid = false;
                ExecuteSpawn(pending.name, pending.anchor, pending.pos[0], pending.pos[1],
                             pending.pos[2], pending.hasScale ? pending.scale : nullptr,
                             &pending.equip);
            }

            if (RawCallbackFn cb = CallbackRef()) {
                cb(param1);
            }
        }
    }
};

// const char* Enemy::getEquippedItem(Enemy* this, int slot) at 0x0201fa8c
// (Switch 0x7100014f04). The derivation and the open questions are in the
// "Spawning an enemy with equipment" block above.
//
// The signature is the one both platforms agree on, (this, int). The V208
// decompilation shows eight leading float parameters. They are Ghidra
// propagating f1..f8 through the call and never read; the real arguments are
// r3 = this and r4 = slot, and the body uses exactly those two (param_9 and
// param_10 in the listing).
//
// The return value must stay valid after the hook returns, because
// createEquipments passes it on to the level-sensor rewrite and to
// 0x037b5cb8. So it points into the static SpawnEquip, never at a local.
// (A new armed spawn arriving on another thread in the middle of that one
// loop iteration could overwrite the string under it. The window is one
// actor creation long and needs two armed spawns within it, which the
// IsWeaponRequestPending wait in callers already rules out.)
//
// Behaviour:
//   * no armed request, or not our enemy -> the game's own answer (Orig);
//   * our enemy, first call              -> claim; if the request carries an
//                                           arrow, register it in
//                                           ArrowOverrideTable for this enemy;
//   * our enemy, slot with a name        -> that name;
//   * our enemy, empty slot              -> the game's own answer, so anything
//                                           it would have equipped anyway is
//                                           kept (normally nothing, see above);
//   * our enemy, slot 5                  -> request finished, cleared.
//
// Only createEquipments and the 0x02482838 override were found calling this
// virtual, both from init, so the hook costs nothing outside actor creation.
// The prologue is checked by the hook manager at install like any other hook.
WIIXL_HOOK_DEFINE_TRAMPOLINE(EnemyGetEquippedItemHook) {
    static const char* Callback(void* enemy, int slot) {
        SpawnEquip& e = SpawnEquipRef();
        EquipDebug& dbg = EquipDebugRef();
        dbg.getterCalls = dbg.getterCalls + 1;
        if (e.valid && enemy && slot >= 0 && slot < static_cast<int>(kEnemyEquipSlotCount)) {
            dbg.getterWhileArmed = dbg.getterWhileArmed + 1;
            bool mine = e.claimedBy == enemy;
            if (!mine && !e.claimedBy) {
                const char* name = RawActorName(enemy);
                CopyActorName(dbg.lastAskedWhileArmed, sizeof(dbg.lastAskedWhileArmed),
                              name ? name : "(unreadable name)");
                if (name && std::strncmp(name, e.actorName, sizeof(e.actorName)) == 0) {
                    e.claimedBy = enemy;
                    mine = true;
                    dbg.claims = dbg.claims + 1;
                    // The arrow outlives the request: getArrowName is asked
                    // again whenever the enemy deals with arrows, long after
                    // this init-time loop is over. See ARROWS.
                    if (e.req.arrow[0]) AddArrowOverride(enemy, e.actorName, e.req.arrow);
                }
            }
            if (mine) {
                const char* item = e.req.items[slot][0] ? e.req.items[slot] : nullptr;
                if (slot == static_cast<int>(kEnemyEquipSlotCount) - 1) {
                    e.valid = false;
                    e.claimedBy = nullptr;
                }
                // Our slot wins; an empty one falls through to the game, so
                // anything else it would have equipped is kept.
                if (item) {
                    dbg.slotsAnswered = dbg.slotsAnswered + 1;
                    dbg.lastSlotAnswered = slot;
                    return item;
                }
            }
        }
        return Orig(enemy, slot);
    }
};

// bool Enemy::getArrowName(Enemy* this, sead::BufferedSafeString* out) at
// 0x0201901c (Switch 0x7100016368), vtable +0x52c. See ARROWS in the
// "Spawning an enemy with equipment" block for how it was found and who
// calls it.
//
// Signature: r3 = this, r4 = out; returns non-zero when `out` was written.
// `out` is the Wii U BufferedSafeString layout {char* buffer; vtable;
// s32 bufferSize}, so the answer is copied into word 0's buffer, clamped to
// word 2's size and terminated - exactly what the original body does with
// the param value it reads. The caller owns the buffer (the callers found
// all pass a 32-byte stack buffer), so nothing here has to stay alive.
//
// Behaviour:
//   * enemy in ArrowOverrideTable, name still matching -> our arrow, true;
//   * anything else                                    -> the game (Orig),
//     which for a spawned enemy is the AIProgram default or false.
//
// Called by every arrow-related query on every enemy, the player's own arrow
// lookups excepted (0x024a1274 handles the player itself). The table walk is
// 32 pointer compares, and nothing else happens on a miss.
WIIXL_HOOK_DEFINE_TRAMPOLINE(EnemyGetArrowNameHook) {
    static int Callback(void* enemy, void* outString) {
        EquipDebug& dbg = EquipDebugRef();
        dbg.arrowCalls = dbg.arrowCalls + 1;
        if (enemy && outString) {
            ArrowOverrideTable& t = ArrowOverridesRef();
            for (uint32_t i = 0; i < ArrowOverrideTable::kEntries; ++i) {
                ArrowOverride& o = t.e[i];
                if (o.enemy != enemy) continue;
                // Same pointer, different actor: memory reused after the
                // armed enemy was deleted. Leave it to the game.
                const char* name = RawActorName(enemy);
                if (!name || std::strncmp(name, o.actorName, sizeof(o.actorName)) != 0) break;

                auto* words = static_cast<uint32_t*>(outString);
                char* buffer = reinterpret_cast<char*>(static_cast<uintptr_t>(words[0]));
                const int32_t cap = static_cast<int32_t>(words[2]);
                if (!buffer || cap <= 0) break;
                CopyActorName(buffer, static_cast<uint32_t>(cap), o.arrow);
                dbg.arrowsAnswered = dbg.arrowsAnswered + 1;
                return 1;
            }
        }
        return Orig(enemy, outString);
    }
};

#endif // !WIIXL_SWITCH

} // namespace impl

class Actor {
public:
    enum class Kind : uint8_t {
        Proc = 0,
        Placement = 1
    };

    Actor() : m_Ptr(nullptr), m_Kind(Kind::Proc) {}
    explicit Actor(void* ptr, Kind kind = Kind::Proc) : m_Ptr(ptr), m_Kind(kind) {}

    bool IsValid() const { return m_Ptr != nullptr; }
    void* GetRaw() const { return m_Ptr; }
    Kind GetKind() const { return m_Kind; }
    bool IsPlacement() const { return m_Kind == Kind::Placement; }
    bool IsDynamic() const { return m_Kind == Kind::Proc; }

    const char* GetName() const {
        if (!m_Ptr || !IsReadablePtr(m_Ptr)) return "(none)";
#if WIIXL_SWITCH
        using GetActorUniqueNameFn = const char* (*)(void* actor);
        auto getUniqueName = WiiXLaunch::GetTargetFunction<GetActorUniqueNameFn>(0x11c9bfc, 0x0);
        const char* name = getUniqueName(m_Ptr);
        return name ? name : "(unnamed)";
#else
        if (m_Kind == Kind::Placement) {
            auto* base = static_cast<const uint8_t*>(m_Ptr);
            const char* const* ppName = reinterpret_cast<const char* const*>(base + 0xd8);
            const char* raw = nullptr;
            if (IsReadablePtr(ppName)) raw = *ppName;
            if (!raw || !IsReadablePtr(raw)) raw = reinterpret_cast<const char*>(base + 0xe4);
            if (!raw || !IsReadablePtr(raw) || raw[0] == '\0') return "(unnamed)";
            return raw;
        }

        static char buf[64];
        constexpr uintptr_t kSafeStringOffset = 0x04;
        constexpr uintptr_t kInlineNameOffset = 0x10;

        auto* base = static_cast<const uint8_t*>(m_Ptr);
        const char* raw = *reinterpret_cast<const char* const*>(base + kSafeStringOffset);
        if (!IsReadablePtr(raw)) raw = reinterpret_cast<const char*>(base + kInlineNameOffset);

        int n = 0;
        for (; n < static_cast<int>(sizeof(buf)) - 1; n++) {
            if (raw[n] == '\0') break;
            buf[n] = raw[n];
        }
        buf[n] = '\0';
        return n > 0 ? buf : "(unnamed)";
#endif
    }

    // Asks the game to delete this actor, the same way it deletes its own:
    // ksys::act::BaseProc::deleteLater(DeleteReason). Returns whether the
    // request was accepted - it is refused if the actor is already being
    // deleted or already flagged for it, which is a normal outcome rather
    // than an error.
    //
    // Unlike spawning, this is confirmed on both platforms:
    //   Switch  0x11b9da4  - symbolised in the 1.5.0 binary
    //   Wii U   0x0378a374 - V208, matched against that symbolised copy: same
    //                        early-outs on the state byte and delete flag, the
    //                        same name-string vfunc called twice, the same
    //                        BaseProcMgr singleton and high-priority-thread
    //                        check, the same lock-then-recheck of both
    //                        conditions, and the same virtual dispatch of the
    //                        reason. Only struct/vtable offsets differ, as
    //                        expected between a 64- and a 32-bit build.
    //
    // The Wii U side is corroborated by the game's own use of it: 0x0378a70c
    // calls it as deleteLater(proc, 1) and deleteLater(proc, 2) on the failure
    // paths of actor creation, which fixes both the argument order and the
    // meaning of the second parameter.
    //
    // The Switch address is derived from the symbolised binary rather than
    // observed running; the identity is certain, the runtime behaviour has not
    // been exercised there yet.
    //
    // Confirmed on Wii U by deleting the player's equipped sword: the weapon
    // leaves Link's hand. Its inventory entry stays, which is correct - that is
    // save data, not the actor.
    //
    // Confirmed to remove a spawned weapon from the world as well, once that
    // weapon has been woken (see Spawn). The one case where the proc dies and
    // the model stays is an actor the game never places in the world - armour -
    // which is a property of that actor, not of this call. See Spawn below.
    //
    // The proc's state flags at +0x64 drive this and two neighbouring
    // transitions, via 0x0378a1b8(proc, bit) which sets a bit and queues the
    // proc for state processing:
    //
    //   bit 0  request deletion (what this uses)
    //   bit 1  wake:  Sleep -> Calc, and it stays awake
    //   bit 2  sleep: Calc  -> Sleep
    static constexpr bool SupportsDelete = true;

    bool Delete(uint32_t reason = 0) const {
        if (!m_Ptr) return false;
        using DeleteLaterFn = int (*)(void* proc, uint32_t reason);
        auto deleteLater = WiiXLaunch::GetTargetFunction<DeleteLaterFn>(0x11b9da4, 0x0378a374);
        return deleteLater(m_Ptr, reason) != 0;
    }

    // Actor creation ("spawn an actor near this one") is only confirmed on
    // Wii U/Cemu - no Switch equivalent was ever RE'd (see
    // handwritten-symbols-botw.csv). Spawn() is a silent, safe no-op on
    // Switch (returns false) rather than reading/writing an unconfirmed
    // offset - check SupportsSpawn if you need to branch mod behavior on it.
    static constexpr bool SupportsSpawn = !WIIXL_SWITCH;

    // Installs the spawn/player-tick plumbing hooks. Call once from WiiXLaunch_Init().
    static void Init() {
#if !WIIXL_SWITCH
        impl::SpawnFlushHook::Install(0x0, 0x024adac8);
        impl::RequestCreateBaseProcFixHook::Install(0x0, 0x03948cb8);
        // Spawn(..., weapon) - see impl::SpawnEquip. Passes through untouched
        // while no armed request exists.
        impl::EnemyGetEquippedItemHook::Install(0x0, impl::kEnemyGetEquippedItemAddr);
        // Spawn(..., SpawnLoadout{.arrow = ...}) - see ARROWS in impl. Passes
        // through untouched for every enemy not in ArrowOverrideTable.
        impl::EnemyGetArrowNameHook::Install(0x0, impl::kEnemyGetArrowNameAddr);
#endif
    }

    // Registers a callback fired from an actor's per-frame update.
    //
    // The Actor passed in is the actor that is currently ticking, NOT the
    // player - see the note on SpawnFlushHook. It fires many times per frame,
    // for different actors, on several threads. Treat it as "a safe point to
    // run per-frame work from", and fetch anything specific yourself.
    using PlayerCallbackFn = void (*)(const Actor& tickingActor);
    static void OnUpdate(PlayerCallbackFn callback) {
#if !WIIXL_SWITCH
        static PlayerCallbackFn s_Callback = nullptr;
        s_Callback = callback;
        impl::SpawnFlushHook::CallbackRef() = [](void* ptr) {
            if (s_Callback) s_Callback(Actor(ptr));
        };
#else
        (void)callback;
#endif
    }

    // Queues actorName to spawn near anchor at (x, y, z); the actual
    // creation happens on the next safe per-frame tick (see SpawnFlushHook).
    // Returns false immediately on Switch (SupportsSpawn == false).
    //
    // The return value says the request was queued, not that an actor exists,
    // and there is still no way to get the created actor back: the handle only
    // ever carries request state, and the proc is stored on the pooled
    // BaseProcUnit instead (see impl::SpawnedHandle for both layouts).
    //
    // Worth knowing before relying on this: 0x0378a70c only runs the create
    // path when the handle's word 0 is 1, and takes a deleteLater branch
    // otherwise. This handle is left zeroed, so that condition is not currently
    // met. Spawning does produce visible actors regardless, so the path
    // actually taken has not been fully pinned down.
    //
    // **Spawn what the game itself puts in the world, and it just works.**
    // Confirmed on Wii U with a weapon, an animal and an enemy: a spawned
    // Enemy_Bokoblin_Junior runs its AI, animates, reacts to the player and
    // fights. A spawned weapon falls under gravity with its proc position
    // tracking the model and brings its own sub-actors along (a sword spawns
    // its sheath, which the game then cleans up itself). Actor::Delete removes
    // any of them cleanly. Nothing extra is needed to make this happen.
    //
    // Actors that the game never places in the world do not. Armour is the
    // clear case: an Armor_* actor binds to a skeleton - the player's, or the
    // pause menu doll's - and nothing in the game ever creates one as a
    // free-standing world actor. Spawn one and it *renders*, which makes it
    // look like it worked, but it has no world lifecycle:
    //
    //   * Writing the proc's position moves the field and not the model.
    //   * Deleting the proc destroys it - confirmed by watching its memory get
    //     handed to another actor - and the model stays on screen.
    //
    // That is not a fault in this code. Such an actor is created correctly:
    // compared field for field against the armour the pause menu builds, ours
    // matches on state, job bits, flags, vtable and unit, both at creation and
    // at deletion. There is simply no teardown path for a world armour model,
    // because the game never makes one.
    //
    // So: fine for weapons, animals, enemies, NPCs, props. For armour, expect
    // a model that renders once and can never be moved or removed.
    //
    // Armour also never leaves the Sleep state - it goes Init -> Sleep and
    // never reaches Calc. That is the same story rather than a separate one:
    // an actor with no world behaviour has nothing to run. Actors that do have
    // behaviour reach Calc by themselves, with no help from the caller.
    //
    // EQUIPMENT. Two optional forms, both ending in the same place:
    //
    //   Spawn(name, anchor, x, y, z, "Weapon_Sword_001")
    //       a melee weapon only - shorthand for SpawnLoadout{.weapon = ...};
    //   Spawn(name, anchor, x, y, z, SpawnLoadout{...})
    //       weapon, shield, bow, arrow type, or raw slots - see SpawnLoadout.
    //
    // Leaving it out (or passing nullptr / an empty SpawnLoadout) spawns
    // exactly as before. The weapon form was added with a default argument so
    // every existing caller, including the botw.actor surface's AcSpawn, is
    // unchanged.
    //
    // Equipment is not written into the creation params, since no param
    // exists for it. It arms impl::EnemyGetEquippedItemHook, which answers the
    // enemy's own weapon query during its init, and, for an arrow,
    // impl::EnemyGetArrowNameHook, which answers the arrow query for the
    // enemy's whole life. The game creates and equips the weapons itself. See
    // the "Spawning an enemy with equipment" block in impl for the RE, the
    // addresses, the slot layout and what is not yet confirmed.
    //
    // Things to know:
    //   * only an Enemy asks, so on anything else it has no effect;
    //   * weapons may come out upgraded by the level sensor, as for a placed
    //     enemy;
    //   * one armed request at a time: a second armed Spawn before the first
    //     enemy has initialised replaces the first request, and that enemy
    //     comes up unarmed. Wait on IsWeaponRequestPending() between armed
    //     spawns.
    static bool Spawn(const char* actorName, const Actor& anchor, float x, float y, float z,
                      const char* weapon = nullptr) {
        SpawnLoadout loadout;
        loadout.weapon = weapon;
        return Spawn(actorName, anchor, x, y, z, loadout);
    }

    static bool Spawn(const char* actorName, const Actor& anchor, float x, float y, float z,
                      const SpawnLoadout& loadout) {
#if WIIXL_SWITCH
        (void)actorName; (void)anchor; (void)x; (void)y; (void)z; (void)loadout;
        return false;
#else
        impl::PendingSpawn& pending = impl::PendingSpawnRef();
        // The slot holds exactly ONE request, and SpawnFlushHook drains at most
        // one per invocation. Overwriting an unconsumed request destroys it,
        // silently: the caller was told true, so it marks the actor spawned and
        // never retries, and the actor never appears. Refusing while the slot is
        // busy is what lets a caller queue N actors by retrying across N ticks.
        if (pending.valid) return false;
        pending.valid = true;
        impl::CopyActorName(pending.name, sizeof(pending.name), actorName);
        pending.anchor = anchor.GetRaw();
        pending.pos[0] = x;
        pending.pos[1] = y;
        pending.pos[2] = z;
        pending.hasScale = false;
        impl::BuildEquipRequest(pending.equip, loadout);
        return true;
#endif
    }

    // Spawn() with a per-axis scale, written into the creation params as "@S".
    //
    // The scale a map placement carries is a vec3 and so is this: the game's
    // own uniform-scale helper (0x037b55a4) splats a single float into three
    // and writes the same key, and the second overload in the decompilation
    // takes a sead::Vector3f directly. AirWallCurseGanon is placed at
    // {8, 5, 6} in E-4, so non-uniform is what the game itself ships.
    //
    // NOT MEASURED, and the reason to be careful with it: the physics side of
    // scaling that this repo could read is uniform-only - phys::RigidBody and
    // phys::BoxShape both take setScale(float), which multiplies all three
    // extents by the same number. phys::BoxShape::setExtents does take a vec3,
    // so a per-axis box IS representable and the placement data says the game
    // builds them, but the path from "@S" to those extents was not traced. If a
    // non-uniform wall comes out cubic, that is where it went - fall back to
    // equal components and more, smaller panels.
    //
    // Scale multiplies the actor's own shape, it does not replace it. An
    // AirWallCurseGanon is a 2 x 2 x 2 box, so {t, h, w} gives 2t x 2h x 2w.
    //
    // One request is queued at a time, same as Spawn - a caller placing
    // several actors issues one per tick.
    //
    // Equipment: optional, in the same two forms as Spawn.
    static bool SpawnScaled(const char* actorName, const Actor& anchor, float x, float y, float z,
                            float scaleX, float scaleY, float scaleZ,
                            const char* weapon = nullptr) {
        SpawnLoadout loadout;
        loadout.weapon = weapon;
        return SpawnScaled(actorName, anchor, x, y, z, scaleX, scaleY, scaleZ, loadout);
    }

    static bool SpawnScaled(const char* actorName, const Actor& anchor, float x, float y, float z,
                            float scaleX, float scaleY, float scaleZ,
                            const SpawnLoadout& loadout) {
#if WIIXL_SWITCH
        (void)actorName; (void)anchor; (void)x; (void)y; (void)z;
        (void)scaleX; (void)scaleY; (void)scaleZ; (void)loadout;
        return false;
#else
        impl::PendingSpawn& pending = impl::PendingSpawnRef();
        // The slot holds exactly ONE request, and SpawnFlushHook drains at most
        // one per invocation. Overwriting an unconsumed request destroys it,
        // silently: the caller was told true, so it marks the actor spawned and
        // never retries, and the actor never appears. Refusing while the slot is
        // busy is what lets a caller queue N actors by retrying across N ticks.
        if (pending.valid) return false;
        pending.valid = true;
        impl::CopyActorName(pending.name, sizeof(pending.name), actorName);
        pending.anchor = anchor.GetRaw();
        pending.pos[0] = x;
        pending.pos[1] = y;
        pending.pos[2] = z;
        pending.hasScale = true;
        pending.scale[0] = scaleX;
        pending.scale[1] = scaleY;
        pending.scale[2] = scaleZ;
        impl::BuildEquipRequest(pending.equip, loadout);
        return true;
#endif
    }

    // Whether an armed Spawn is still waiting for its enemy to ask for its
    // equipment, i.e. impl::SpawnEquip is armed and its claimant has not yet
    // reached slot 5. "Armed" means any equipment at all, an arrow-only
    // SpawnLoadout included. Once the enemy has claimed the request, its
    // arrow lives on in impl::ArrowOverrideTable and no longer counts here.
    //
    // Only one armed request is held at a time and a new armed Spawn replaces
    // it, so a caller spawning several armed enemies waits for this to clear
    // between them. src/main.cpp's ExecutePendingSpawn is the reference user.
    //
    // It is false right after Spawn() returns and turns true only once
    // SpawnFlushHook has actually run the spawn, because the request is armed
    // inside ExecuteSpawn. Spawn() refuses a second request until then anyway,
    // so a caller retrying Spawn() and checking this never loses one.
    //
    // It can stay true for good: when the enemy never builds its equipment
    // (the weapon-creation failure cases in impl::SpawnEquip), or when the name never
    // matched an enemy (a typo, or a non-enemy actor given a weapon). So a
    // caller should give up after a while rather than wait forever; the next
    // armed Spawn simply replaces the stale request.
    static bool IsWeaponRequestPending() {
#if WIIXL_SWITCH
        return false;
#else
        return impl::SpawnEquipRef().valid;
#endif
    }

#if !WIIXL_SWITCH
    // Diagnostic counters of the equipment hooks - see impl::EquipDebug for
    // what each one means. Read-only; log them from a tick to see how far an
    // equipped spawn got:
    //   arms == 0            the spawn never carried equipment
    //   getterWhileArmed == 0  no enemy asked for equipment after arming
    //   claims == 0          enemies asked, but lastAskedWhileArmed is not
    //                        the name that was requested
    //   slotsAnswered > 0    names were handed to the game; if the enemy is
    //                        still unarmed, the game dropped them after that
    static const impl::EquipDebug& GetEquipDebug() { return impl::EquipDebugRef(); }
#endif

    // Health / Life accessors (Wii U / Cemu confirmed):
    // On Wii U, Actor+0xe8 is the primary actor vtable pointer.
    //
    // Byte offsets here are right; the index numbers this comment used to give
    // were not. A Wii U vtable entry is EIGHT bytes - {s16 delta; s16 index;
    // void* fn} - with the function pointer at entry+4, so virtual index N
    // lives at byte offset N*8 + 4. Dividing an offset by 4 produces an index
    // that does not exist, which is what "index 61" and "index 175" were.
    //
    //   +0xf4  = 30*8 + 4  -> index 30, GetMaxLife(actor) -> int
    //   +0x2bc = 87*8 + 4  -> index 87, GetCurrentLifePtr(actor) -> int*
    //   +0x2ac = 85*8 + 4  -> index 85, setMtx  (see kSetMtxVtableSlot)
    //   +0x2a4 = 84*8 + 4  -> index 84, updateMtxFromPhysics
    //
    // Verified by reading a live player's vtable: every entry showed delta=0
    // and a .text pointer at +4, and both slots below landed on real functions.
    //
    // Indexing vtable[byteOffset / 4] as a void** still lands on the right word
    // - that is the function pointer's own address - so the code below is
    // correct as written; only the stated indices were wrong.
    // Capability flag, per the module convention - a mod asks rather than
    // discovering by getting zeroes back. The vtable slots above are Wii U/Cemu
    // confirmed; no Switch equivalent was ever RE'd, so the accessors return 0
    // there instead of reading an unconfirmed offset.
    //
    // It matters more here than for most flags, because 0 life is a LEGITIMATE
    // value. Without the flag, "this platform cannot read life" and "this actor
    // is dead" are the same answer.
    static constexpr bool SupportsLife = !WIIXL_SWITCH;

    int GetCurrentLife() const {
#if !WIIXL_SWITCH
        if (!m_Ptr) return 0;
        uint8_t* ptr = static_cast<uint8_t*>(m_Ptr);
        void** vtable = *reinterpret_cast<void***>(ptr + 0xe8);
        if (!vtable) return 0;
        uintptr_t vtAddr = reinterpret_cast<uintptr_t>(vtable);
        if (vtAddr < 0x02000000 || vtAddr > 0x10600000) return 0;

        using GetLifePtrFn = int* (*)(void* actor);
        auto fn = reinterpret_cast<GetLifePtrFn>(vtable[0x2bc / 4]);
        if (!fn) return 0;
        uintptr_t fnAddr = reinterpret_cast<uintptr_t>(fn);
        if (fnAddr < 0x02000000 || fnAddr > 0x04000000) return 0;

        int* pLife = fn(m_Ptr);
        if (!pLife) return 0;
        uintptr_t lifeAddr = reinterpret_cast<uintptr_t>(pLife);
        if (lifeAddr < 0x10000000 || lifeAddr > 0xa0000000 || (lifeAddr & 3) != 0) return 0;

        return *pLife;
#else
        return 0;
#endif
    }

    void SetCurrentLife(int life) {
#if !WIIXL_SWITCH
        if (!m_Ptr) return;
        uint8_t* ptr = static_cast<uint8_t*>(m_Ptr);
        void** vtable = *reinterpret_cast<void***>(ptr + 0xe8);
        if (!vtable) return;
        uintptr_t vtAddr = reinterpret_cast<uintptr_t>(vtable);
        if (vtAddr < 0x02000000 || vtAddr > 0x10600000) return;

        using GetLifePtrFn = int* (*)(void* actor);
        auto fn = reinterpret_cast<GetLifePtrFn>(vtable[0x2bc / 4]);
        if (!fn) return;
        uintptr_t fnAddr = reinterpret_cast<uintptr_t>(fn);
        if (fnAddr < 0x02000000 || fnAddr > 0x04000000) return;

        int* pLife = fn(m_Ptr);
        if (!pLife) return;
        uintptr_t lifeAddr = reinterpret_cast<uintptr_t>(pLife);
        if (lifeAddr < 0x10000000 || lifeAddr > 0xa0000000 || (lifeAddr & 3) != 0) return;

        *pLife = life;
#else
        (void)life;
#endif
    }

    int GetMaxLife() const {
#if !WIIXL_SWITCH
        if (!m_Ptr) return 0;
        uint8_t* ptr = static_cast<uint8_t*>(m_Ptr);
        void** vtable = *reinterpret_cast<void***>(ptr + 0xe8);
        if (!vtable) return 0;
        uintptr_t vtAddr = reinterpret_cast<uintptr_t>(vtable);
        if (vtAddr < 0x02000000 || vtAddr > 0x10600000) return 0;

        using GetMaxLifeFn = int (*)(void* actor);
        auto fn = reinterpret_cast<GetMaxLifeFn>(vtable[0xf4 / 4]);
        if (!fn) return 0;
        uintptr_t fnAddr = reinterpret_cast<uintptr_t>(fn);
        if (fnAddr < 0x02000000 || fnAddr > 0x04000000) return 0;

        return fn(m_Ptr);
#else
        return 0;
#endif
    }

    // Max life cannot be set through the actor. getMaxLife() reads a cache at
    // actor+0x1320; the authoritative, save-backed value is the GameData flag
    // MaxHartValue, and nothing re-syncs the cache from it. Writing here shows
    // the new value for a frame, then reverts, and the HUD plays the
    // heart-container-removed (Horned Statue) effect on the way back down.
    //
    // Use GameData::SetMaxLife (gamedata.hpp), which calls the game's own
    // PlayerInfo::setMaxHeartValue - flag and caches together. Kept here as an
    // explicit no-op so this does not get reimplemented the wrong way again.
    static constexpr bool SupportsSetMaxLife = false;

    bool SetMaxLife(int maxLife) {
        (void)maxLife;
        return false;
    }

    // Convenience helpers converting between integer Life units (4 per heart) and floating-point hearts
    float GetCurrentHearts() const { return GetCurrentLife() / 4.0f; }
    void SetCurrentHearts(float hearts) { SetCurrentLife(static_cast<int>(hearts * 4.0f + 0.5f)); }

    float GetMaxHearts() const { return GetMaxLife() / 4.0f; }
    bool SetMaxHearts(float hearts) { return SetMaxLife(static_cast<int>(hearts * 4.0f + 0.5f)); }



    struct Vec3 {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    // --- ID & State Queries (Wii U / Cemu confirmed) ---
    // Offset +0x50 holds the uint32_t BaseProc/Actor ID.
    // Offset +0x54 holds the lifecycle state byte: 0 = Init, 1 = Calc/Active, 2 = Sleep, 3 = Deleted/Dying.

    uint32_t GetId() const {
#if !WIIXL_SWITCH
        if (!m_Ptr || !IsReadablePtr(m_Ptr)) return 0;
        if (m_Kind == Kind::Placement) {
            return *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(m_Ptr) + 0x00);
        }
        return *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(m_Ptr) + 0x50);
#else
        return 0;
#endif
    }

    uint8_t GetState() const {
#if !WIIXL_SWITCH
        if (!m_Ptr || !IsReadablePtr(m_Ptr)) return 0;
        if (m_Kind == Kind::Placement) return 1;
        return *reinterpret_cast<const uint8_t*>(static_cast<const uint8_t*>(m_Ptr) + 0x54);
#else
        return 0;
#endif
    }

    bool IsActive() const { return GetState() == 1; }
    bool IsSleeping() const { return GetState() == 2; }
    bool IsDying() const { return GetState() == 3; }

    // --- Position & Rotation (Wii U / Cemu confirmed) ---
    // Dynamic Procs: the sead::Matrix34f at +0x1F8. Position is its translation
    // column (+0x204/+0x214/+0x224); rotation is its 3x3 basis. See the offset
    // notes and the setMtx write-up in impl above - in particular, why writing
    // the position fields directly never moved an actor.
    // Map Placements: +0x14 (X), +0x18 (Y), +0x1C (Z); +0x30/+0x34/+0x38 rotation.

    static constexpr bool SupportsTransform = !WIIXL_SWITCH;
    static constexpr bool SupportsPosition = !WIIXL_SWITCH;
    static constexpr bool SupportsRotation = !WIIXL_SWITCH;

    // Raw access to the actor's sead::Matrix34f, row-major, 12 floats.
    // Placements have no matrix, so both return false for them.
    bool GetMatrix(float outMtx34[12]) const {
#if !WIIXL_SWITCH
        if (!m_Ptr || !IsReadablePtr(m_Ptr) || m_Kind == Kind::Placement) return false;
        const auto* src = reinterpret_cast<const float*>(
            static_cast<const uint8_t*>(m_Ptr) + impl::kActorMatrixOffset);
        for (uint32_t i = 0; i < impl::kActorMatrixFloats; ++i) outMtx34[i] = src[i];
        // An actor whose stored transform is not finite has no transform worth
        // reading, and must not have one written back - see IsFiniteMatrix.
        // SetPosition and SetRotation both read through here, so refusing it
        // here is what keeps them off those actors.
        return IsFiniteMatrix(outMtx34);
#else
        (void)outMtx34;
        return false;
#endif
    }

    // Hands the matrix to ksys::act::Actor::setMtx so the actor's published
    // transform at +0x22C is updated too - a straight write to +0x1F8 is
    // reverted by the actor's own update. velocity is optional (+0x274).
    bool SetMatrix(const float mtx34[12], const Vec3* velocity = nullptr) const {
#if !WIIXL_SWITCH
        // Re-checked here and not only at the traversal: this is the call that
        // writes 48 bytes at +0x1F8 and again at +0x22C, so it refuses anything
        // that does not look like a real BaseProc regardless of how the Actor
        // was constructed.
        if (!IsPlausibleProc(m_Ptr) || m_Kind == Kind::Placement) return false;
        // Never hand a non-finite matrix to the game, whatever produced it.
        if (!IsFiniteMatrix(mtx34)) return false;
        auto setMtx = WiiXLaunch::GetTargetFunction<impl::SetActorMtxFn>(0x0, impl::kSetActorMtxAddr);
        const float vel[3] = { velocity ? velocity->x : 0.0f,
                               velocity ? velocity->y : 0.0f,
                               velocity ? velocity->z : 0.0f };
        setMtx(m_Ptr, mtx34, velocity ? vel : nullptr);
        return true;
#else
        (void)mtx34; (void)velocity;
        return false;
#endif
    }

    // Whether this actor's transform is parented to something else - a held
    // weapon, worn armour, anything riding or mounted.
    //
    // Not a heuristic: +0x3B8 is the exact field setMtx branches on. When it
    // is null 0x037986e4 writes the actor's own matrix straight to +0x22C;
    // when it is not, the write is routed through the attach frame via
    // 0x034d3d40, which combines it against the parent's transform. Moving an
    // attached actor independently of its parent is therefore a different
    // operation from moving a free-standing one, and worth being able to
    // exclude.
    bool IsAttached() const {
#if !WIIXL_SWITCH
        if (!IsPlausibleProc(m_Ptr) || m_Kind == Kind::Placement) return false;
        return *reinterpret_cast<void* const*>(
            static_cast<const uint8_t*>(m_Ptr) + 0x3b8) != nullptr;
#else
        return false;
#endif
    }

    // --- Linear velocity (Wii U / Cemu) ---
    //
    // actor+0x25C is a contiguous sead::Vector3f velocity - unlike the
    // position, which is the strided translation column of the matrix at
    // +0x1F8.
    //
    // Identified from 0x0383b398, which feeds actor+0x25C to 0x03c6fcf0 and
    // uses the result as a speed value. 0x03c6fcf0 is
    // sqrt(v[0]^2 + v[1]^2 + v[2]^2), so the field is three floats wide; the
    // same function separately reads +0x25C and +0x264 and takes
    // sqrt(x^2 + z^2) for horizontal speed, which fixes the component order as
    // x at +0x25C, y at +0x260, z at +0x264.
    //
    // Worth knowing which lever this is. Writing a position on an actor the
    // physics drives does not stick - Link's is restored to the bit-identical
    // original on the next tick, measured. Velocity is the input that a
    // character controller integrates, so it is the one that survives, and it
    // is how the game itself makes an actor move rather than teleport.
    //
    // Not to be confused with actor+0x274, the optional third argument of
    // setMtx (see kSetActorMtxAddr); that one is written only when a caller
    // passes it and is used elsewhere as a scale factor.
    static constexpr bool SupportsVelocity = !WIIXL_SWITCH;

    bool GetLinearVelocity(float& x, float& y, float& z) const {
#if !WIIXL_SWITCH
        if (!IsPlausibleProc(m_Ptr) || m_Kind == Kind::Placement) return false;
        const auto* v = reinterpret_cast<const float*>(
            static_cast<const uint8_t*>(m_Ptr) + impl::kVelocityOffset);
        if (!IsFiniteFloat(v[0]) || !IsFiniteFloat(v[1]) || !IsFiniteFloat(v[2])) return false;
        x = v[0]; y = v[1]; z = v[2];
        return true;
#else
        (void)x; (void)y; (void)z;
        return false;
#endif
    }

    Vec3 GetLinearVelocity() const {
        Vec3 v;
        GetLinearVelocity(v.x, v.y, v.z);
        return v;
    }

    // Writes Havok's storage, NOT the actor cache at +0x25c.
    //
    // This wrote the cache until it was measured: the value reads back
    // correctly and moves nothing, because physics recomputes it every frame.
    // See GetHavokMotion for the chain and how it was found.
    // Requests a velocity the way the game does, in Actor::setVelocity
    // (0x03798b60): write the floats, then set the request flag.
    //
    // The flag is not optional. Without it this wrote a field that reads back
    // correctly and moves nothing. Every other candidate found by probing -
    // the Havok motion at motion+0x1b0, RigidBody::setLinearVelocity - is an
    // output or needs a three-part transaction (value, motion flag, and a push
    // onto RigidBodyRequestMgr) that a memory write cannot fake. This one is
    // an input, so a write is all it takes.
    bool SetLinearVelocity(float x, float y, float z) const {
#if !WIIXL_SWITCH
        if (!IsPlausibleProc(m_Ptr) || m_Kind == Kind::Placement) return false;
        if (!IsFiniteFloat(x) || !IsFiniteFloat(y) || !IsFiniteFloat(z)) return false;

        auto* actor = static_cast<uint8_t*>(m_Ptr);
        // The game's own guard, and it is enforced here.
        //
        // Actor::setVelocity refuses unless +0x54 is 0 or 2. Link sits at 1,
        // so the game never sets the PLAYER's velocity this way. Bypassing it
        // was tried: the floats and the request flag were both written, the
        // flag stayed set (nothing consumed it), and the player moved 0.0
        // units on all four axes with verified 0.00 idle drift. So the player
        // has no velocity INPUT to write - his motion comes from the player
        // state machine, and changing it means hooking that, not poking memory.
        //
        // NOT player-specific, despite how that reads. Measured in-process
        // against a live Bokoblin_Junior and two dropped Weapon_* actors, all
        // sitting at state 1: every call was refused, identically to the
        // player, for the identical reason. State 1 (Calc/Active) is the
        // ordinary "alive and doing something" state for essentially every
        // actor while it is up and ticking - so in practice this only ever
        // accepts an actor sitting in Init or Sleep, which is a narrow window,
        // not "any actor except the player" as the guard's framing might
        // suggest. Prefer SetControllerVelocity for anything actually moving.
        //
        // Enforced rather than ignored so this reports honestly: for actors
        // that DO accept a requested velocity (state 0 or 2) it works, and for
        // everything else it returns false instead of claiming success.
        const char state = *reinterpret_cast<const char*>(actor + impl::kVelocityStateOffset);
        if (state != 0 && state != 2) return false;

        auto* v = reinterpret_cast<float*>(actor + impl::kVelocityOffset);
        v[0] = x; v[1] = y; v[2] = z;
        *(actor + impl::kVelocityRequestFlagOffset) = 1;
        return true;
#else
        (void)x; (void)y; (void)z;
        return false;
#endif
    }

    bool SetLinearVelocity(const Vec3& v) const {
        return SetLinearVelocity(v.x, v.y, v.z);
    }

    // Adds to the current velocity rather than replacing it - the usual way to
    // launch an actor without discarding the motion it already had.
    bool AddLinearVelocity(float x, float y, float z) const {
        Vec3 v;
        if (!GetLinearVelocity(v.x, v.y, v.z)) return false;
        return SetLinearVelocity(v.x + x, v.y + y, v.z + z);
    }

    // The actor's physics/motion object, via slot 0x30C of the +0xE8 vtable.
    //
    // Needed because the matrix at +0x1F8 is an *output* for anything the
    // physics drives: writing Link's position through setMtx reads back
    // correctly, survives the rest of the frame, then is restored to the
    // bit-identical original on the next tick. The authoritative position for
    // such an actor lives on this object, not on the actor.
    //
    // Slot 0x30C is where the game gets it: 0x0383b398 calls it on the player
    // actor and reads Vector3f fields at +0xAC and +0xC4 out of the result,
    // and 0x024b0cbc calls it and writes +0x158. Both null-check the return,
    // so an actor without physics gives null here too.
    void* GetPhysicsObject() const {
#if !WIIXL_SWITCH
        if (!IsPlausibleProc(m_Ptr) || m_Kind == Kind::Placement) return nullptr;

        void** vtable = *reinterpret_cast<void***>(static_cast<uint8_t*>(m_Ptr) + 0xe8);
        if (!vtable) return nullptr;

        using GetPhysFn = void* (*)(void* actor);
        auto fn = reinterpret_cast<GetPhysFn>(vtable[0x30c / 4]);
        uintptr_t fnAddr = reinterpret_cast<uintptr_t>(fn);
        if (fnAddr < 0x02000000 || fnAddr > 0x04000000) return nullptr;

        void* phys = fn(m_Ptr);
        return IsReadablePtr(phys) ? phys : nullptr;
#else
        return nullptr;
#endif
    }

    bool GetPosition(float& x, float& y, float& z) const {
#if !WIIXL_SWITCH
        if (!m_Ptr || !IsReadablePtr(m_Ptr)) return false;
        const auto* base = static_cast<const uint8_t*>(m_Ptr);
        if (m_Kind == Kind::Placement) {
            x = *reinterpret_cast<const float*>(base + 0x14);
            y = *reinterpret_cast<const float*>(base + 0x18);
            z = *reinterpret_cast<const float*>(base + 0x1c);
            return true;
        }
        x = *reinterpret_cast<const float*>(base + impl::kPosXOffset);
        y = *reinterpret_cast<const float*>(base + impl::kPosYOffset);
        z = *reinterpret_cast<const float*>(base + impl::kPosZOffset);
        return true;
#else
        (void)x; (void)y; (void)z;
        return false;
#endif
    }

    static bool IsValidActorName(const char* name) {
        if (!name || !IsReadablePtr(name)) return false;
        if (!((name[0] >= 'A' && name[0] <= 'Z') || (name[0] >= 'a' && name[0] <= 'z'))) return false;
        for (size_t i = 0; i < 64; ++i) {
            char c = name[i];
            if (c == '\0') return i >= 2;
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return false;
        }
        return false;
    }

    // Moves a live actor. Reads the actor's current matrix, replaces only its
    // translation column, and hands the whole thing back through setMtx - see
    // impl::kSetActorMtxAddr for why the write has to go that way round.
    //
    // Returns false for a map placement (Kind::Placement). Those entries are
    // the placement *records* the game reads at load time to decide what to
    // spawn, not the spawned actor, so writing their translate moves nothing -
    // it only corrupts the record. Iterate placements to find things; move the
    // dynamic actor the placement produced.
    bool SetPosition(float x, float y, float z) const {
#if !WIIXL_SWITCH
        if (!m_Ptr || !IsReadablePtr(m_Ptr) || m_Kind == Kind::Placement) return false;

        float mtx[12];
        if (!GetMatrix(mtx)) return false;
        mtx[3] = x;
        mtx[7] = y;
        mtx[11] = z;
        return SetMatrix(mtx);
#else
        (void)x; (void)y; (void)z;
        return false;
#endif
    }

    // Places the actor through ksys::act::Actor::setMtx - virtual index 85 -
    // which is what the game's own Warp* actions call.
    //
    // Use this rather than SetPosition for anything physics drives. SetPosition
    // writes the actor side only, and Actor::updateMtxFromPhysics (index 84)
    // republishes the matrix from the character controller or the rigid body
    // every frame, so the write is gone before it is drawn. setMtx writes the
    // physics side itself, which is why it sticks.
    //
    // Called through the vtable on purpose: the player overrides this, and the
    // override also updates facing and his own position copies.
    //
    // Returns false when the actor, its vtable or the slot do not look right -
    // never a blind call into whatever the slot happened to hold, so a game
    // update that moves the layout fails visibly instead of crashing.
    bool SetMtx(const float mtx34[12], bool setActorMtx = true,
                bool refreshPhysics = true) const {
#if !WIIXL_SWITCH
        if (!m_Ptr || !IsReadablePtr(m_Ptr) || m_Kind == Kind::Placement) return false;
        if (!mtx34) return false;

        uintptr_t base = reinterpret_cast<uintptr_t>(m_Ptr);
        uintptr_t vtable = *reinterpret_cast<uintptr_t*>(base + 0xe8);
        if (vtable < 0x10000000 || vtable >= 0xa0000000 || (vtable & 3)) return false;

        uintptr_t fn = *reinterpret_cast<uintptr_t*>(vtable + impl::kSetMtxVtableSlot);
        if (fn < 0x02000020 || fn >= 0x04347c2c || (fn & 3)) return false;

        reinterpret_cast<impl::ActorSetMtxFn>(fn)(
            m_Ptr, mtx34, setActorMtx ? 1 : 0, refreshPhysics ? 1 : 0);
        return true;
#else
        (void)mtx34; (void)setActorMtx; (void)refreshPhysics;
        return false;
#endif
    }

    // Moves the actor, keeping its current facing, through setMtx.
    //
    // Reads the current matrix and replaces only the translation column, so a
    // move does not also spin the actor. This is the one to reach for when
    // SetPosition "works" and then the actor snaps back.
    bool WarpTo(float x, float y, float z) const {
#if !WIIXL_SWITCH
        float mtx[12];
        if (!GetMatrix(mtx)) return false;
        mtx[3] = x;
        mtx[7] = y;
        mtx[11] = z;
        return SetMtx(mtx, true, true);
#else
        (void)x; (void)y; (void)z;
        return false;
#endif
    }

    bool WarpTo(const Vec3& pos) const { return WarpTo(pos.x, pos.y, pos.z); }

    // WarpTo without the physics-instance reset.
    //
    // setMtx's fourth argument resets the physics instance set - cloth,
    // ragdoll, and whatever else the body owns. That is right for a warp
    // across the map, which is why both of the game's warp actions pass it,
    // and wrong for a correction applied every frame: resetting the set
    // continuously destroys transient state. Live symptom - a region pushback
    // built on WarpTo dragged a SWIMMING player to the river bed, because the
    // swim state was being torn down and rebuilt on every single frame.
    //
    // Use this for small, repeated corrections; use WarpTo to teleport.
    bool NudgeTo(float x, float y, float z) const {
#if !WIIXL_SWITCH
        float mtx[12];
        if (!GetMatrix(mtx)) return false;
        mtx[3] = x;
        mtx[7] = y;
        mtx[11] = z;
        return SetMtx(mtx, true, false);
#else
        (void)x; (void)y; (void)z;
        return false;
#endif
    }

    bool NudgeTo(const Vec3& pos) const { return NudgeTo(pos.x, pos.y, pos.z); }

    // The character controller's DESIRED-VELOCITY input.
    //
    // THE one field that actually moves an actor from a mod. Everything else
    // this header can reach is an output: the actor cache at +0x25c is
    // refreshed from physics, and the Havok linear velocity at motion+0x1b0
    // is recomputed by the controller every frame, so writes to either are
    // overwritten before they move anything. This is the value the controller
    // CONSUMES, and a write here survives.
    //
    //   actor +0x3a0  physics component
    //         +0x50   character controller
    //         +0xb0   desired velocity, three floats, world units per second
    //                 (X +0xb0, Y +0xb4, Z +0xb8)
    //
    // MEASURED, in-process, on a live game: writing (25, 0, 0) here every
    // frame launched a standing player east at a realized 24 u/s. A sweep of
    // every float triple on the controller found this to be the only one with
    // that response.
    //
    // Two things it took a while to learn, both of which make this look broken
    // if you get them wrong:
    //
    //   * It must be written EVERY FRAME. A single-frame write does nothing at
    //     all - the controller overwrites it before the physics step.
    //   * It reads ~0 during ordinary locomotion. Normal movement does not
    //     flow through this field, so a mod that waits for it to be non-zero
    //     before acting waits forever. Read motion+0x1b0 (GetHavokVelocity) to
    //     see how fast the actor is ACTUALLY going; write here to change it.
    //
    // Writes must also come from a hook that runs between the owning actor's
    // update and the physics phase - Player::OnTick is such a place. An
    // external write (a debugger, another process) can never land in that
    // window and will appear to do nothing.
    //
    // NOT a player-only field, in principle: any actor with a character
    // controller has one. UNCONFIRMED against a non-player actor, though, and
    // one attempt came back ambiguous rather than negative. Writing from
    // Actor::OnUpdate (gated on the ticking actor matching the target, so the
    // write lands right after that actor's own AI update, mirroring
    // Player::OnTick's position for the player) against a live
    // Enemy_Bokoblin_Junior: the pointer chain resolved and the write
    // reported success, but zero displacement was measured over one second -
    // and a second attempt found Actor::OnUpdate's underlying hook
    // (SpawnFlushHook, see impl:: below) never fired for that same actor at
    // all in over three seconds, despite firing normally that same session
    // for weapons and props. So it is not established whether the write was
    // clobbered, the target actor simply wasn't moving regardless (a Junior
    // variant can be idle/stationary), or the update hook does not reliably
    // reach AI-driven actors the way it reaches weapons/physics props. Held
    // open rather than guessed at - see the SpawnFlushHook comment for what
    // it has and has not actually been shown to cover.
    //
    // KNOWN LIMIT: shield surfing overrides this every frame, and overrides a
    // direct motion+0x1b0 write too. A surfing player cannot be steered by
    // velocity through any path found so far.
    float* GetControllerVelocityPtr() const {
#if !WIIXL_SWITCH
        if (!IsPlausibleProc(m_Ptr) || m_Kind == Kind::Placement) return nullptr;
        uint8_t* actor = static_cast<uint8_t*>(m_Ptr);

        void* physics = *reinterpret_cast<void**>(actor + 0x3a0);
        if (!IsReadablePtr(physics)) return nullptr;

        void* controller = *reinterpret_cast<void**>(
            static_cast<uint8_t*>(physics) + 0x50);
        if (!IsReadablePtr(controller)) return nullptr;

        return reinterpret_cast<float*>(
            static_cast<uint8_t*>(controller) + impl::kControllerVelocityOffset);
#else
        return nullptr;
#endif
    }

    // Reads the desired velocity. Remember this is the controller's INPUT and
    // reads ~0 during ordinary movement - GetHavokVelocity is what reports how
    // fast the actor is really going.
    bool GetControllerVelocity(float& x, float& y, float& z) const {
#if !WIIXL_SWITCH
        const float* v = GetControllerVelocityPtr();
        if (!v) return false;
        if (!IsFiniteFloat(v[0]) || !IsFiniteFloat(v[1]) || !IsFiniteFloat(v[2]))
            return false;
        x = v[0]; y = v[1]; z = v[2];
        return true;
#else
        (void)x; (void)y; (void)z;
        return false;
#endif
    }

    // Writes the desired velocity. Call EVERY FRAME for as long as the effect
    // is wanted; one call does nothing.
    bool SetControllerVelocity(float x, float y, float z) const {
#if !WIIXL_SWITCH
        float* v = GetControllerVelocityPtr();
        if (!v) return false;
        v[0] = x; v[1] = y; v[2] = z;
        return true;
#else
        (void)x; (void)y; (void)z;
        return false;
#endif
    }

    // The character controller's desired-DIRECTION input - see
    // impl::kControllerDirectionOffset for the RE evidence and the caveat
    // that this is UNCONFIRMED against a live game. Same physics/controller
    // chain as GetControllerVelocityPtr, just a different field on the same
    // object, so the null-checks and their reasons are identical.
    float* GetControllerDirectionPtr() const {
#if !WIIXL_SWITCH
        if (!IsPlausibleProc(m_Ptr) || m_Kind == Kind::Placement) return nullptr;
        uint8_t* actor = static_cast<uint8_t*>(m_Ptr);

        void* physics = *reinterpret_cast<void**>(actor + 0x3a0);
        if (!IsReadablePtr(physics)) return nullptr;

        void* controller = *reinterpret_cast<void**>(
            static_cast<uint8_t*>(physics) + 0x50);
        if (!IsReadablePtr(controller)) return nullptr;

        return reinterpret_cast<float*>(
            static_cast<uint8_t*>(controller) + impl::kControllerDirectionOffset);
#else
        return nullptr;
#endif
    }

    // Reads the desired direction. UNCONFIRMED whether this reads back
    // anything meaningful outside of an actor that is actively steering
    // toward a target via the AI action this was traced from - see
    // impl::kControllerDirectionOffset.
    bool GetControllerDirection(float& x, float& y, float& z) const {
#if !WIIXL_SWITCH
        const float* v = GetControllerDirectionPtr();
        if (!v) return false;
        if (!IsFiniteFloat(v[0]) || !IsFiniteFloat(v[1]) || !IsFiniteFloat(v[2]))
            return false;
        x = v[0]; y = v[1]; z = v[2];
        return true;
#else
        (void)x; (void)y; (void)z;
        return false;
#endif
    }

    // Writes the desired direction. Pass a unit vector - the game's own
    // writer always does, and nothing here normalises it for you.
    // UNCONFIRMED whether a write here alone turns a live actor; see
    // impl::kControllerDirectionOffset before relying on this.
    bool SetControllerDirection(float x, float y, float z) const {
#if !WIIXL_SWITCH
        float* v = GetControllerDirectionPtr();
        if (!v) return false;
        if (!IsFiniteFloat(x) || !IsFiniteFloat(y) || !IsFiniteFloat(z)) return false;
        v[0] = x; v[1] = y; v[2] = z;
        return true;
#else
        (void)x; (void)y; (void)z;
        return false;
#endif
    }

    // Havok's own motion object, where linear velocity actually lives.
    //
    // The actor caches velocity at +0x25c (kVelocityOffset), refreshed every
    // frame from physics by 0x037966bc - so that field READS correctly but
    // cannot be written: a write changes what reads it that frame (the camera
    // visibly shifted) and is overwritten before anything that moves the actor
    // sees it. Measured live - 121 sustained writes of 25.0 moved the player
    // zero units.
    //
    // The storage behind it is reached by the chain 0x037966bc itself walks:
    //
    //   actor +0x3a0  physics component
    //         +0x50   character controller   (0x037966a8 returns this)
    //         +0x04   rigid body             (0x0344a890 -> 0x03488aec)
    //         +0x70   hkpMotion POINTER
    //         +0x1b0  linear velocity, three floats
    //
    // The final dereference is why +0x70 is a pointer and not the motion:
    // 0x034aaad0 loads *param_1 first, then reads +0x1b0/+0x1b4/+0x1b8 off it.
    void* GetHavokMotion() const {
#if !WIIXL_SWITCH
        if (!IsPlausibleProc(m_Ptr) || m_Kind == Kind::Placement) return nullptr;
        uint8_t* actor = static_cast<uint8_t*>(m_Ptr);

        void* physics = *reinterpret_cast<void**>(actor + 0x3a0);
        if (!IsReadablePtr(physics)) return nullptr;

        void* controller = *reinterpret_cast<void**>(static_cast<uint8_t*>(physics) + 0x50);
        if (!IsReadablePtr(controller)) return nullptr;

        void* body = *reinterpret_cast<void**>(static_cast<uint8_t*>(controller) + 0x04);
        if (!IsReadablePtr(body)) return nullptr;

        void* motion = *reinterpret_cast<void**>(static_cast<uint8_t*>(body) + 0x70);
        if (!IsReadablePtr(motion)) return nullptr;

        return motion;
#else
        return nullptr;
#endif
    }

    // Reads velocity straight from Havok instead of the actor cache. The two
    // agree while the cache is fresh; this one is also correct mid-frame, and
    // is what a velocity correction must read so it does not act on a value
    // one frame stale.
    bool GetHavokVelocity(float& x, float& y, float& z) const {
#if !WIIXL_SWITCH
        void* motion = GetHavokMotion();
        if (!motion) return false;
        const float* v = reinterpret_cast<const float*>(
            static_cast<uint8_t*>(motion) + 0x1b0);
        if (!IsFiniteFloat(v[0]) || !IsFiniteFloat(v[1]) || !IsFiniteFloat(v[2])) return false;
        x = v[0]; y = v[1]; z = v[2];
        return true;
#else
        (void)x; (void)y; (void)z;
        return false;
#endif
    }

    Vec3 GetPosition() const {
        Vec3 pos;
        GetPosition(pos.x, pos.y, pos.z);
        return pos;
    }

    bool SetPosition(const Vec3& pos) const {
        return SetPosition(pos.x, pos.y, pos.z);
    }

    // Euler XYZ in radians, matching sead::Matrix34f::makeR (R = Rz * Ry * Rx).
    // There is no Euler triple stored on the actor - the previous +0x1E4 /
    // +0x1F4 / +0x234 offsets were three unrelated fields either side of the
    // matrix - so this decomposes the 3x3 basis instead.
    //
    // The offsets and the setMtx round trip are confirmed; the choice of Euler
    // convention is sead's documented one rather than something observed in
    // game, so treat the angles' sign/order as the part still worth checking.
    bool GetRotation(float& x, float& y, float& z) const {
#if !WIIXL_SWITCH
        if (!m_Ptr || !IsReadablePtr(m_Ptr)) return false;
        if (m_Kind == Kind::Placement) {
            const auto* base = static_cast<const uint8_t*>(m_Ptr);
            x = *reinterpret_cast<const float*>(base + 0x30);
            y = *reinterpret_cast<const float*>(base + 0x34);
            z = *reinterpret_cast<const float*>(base + 0x38);
            return true;
        }

        float m[12];
        if (!GetMatrix(m)) return false;

        // m[8] = m[2][0] = -sin(y)
        float sy = -m[8];
        if (sy > 1.0f) sy = 1.0f;
        if (sy < -1.0f) sy = -1.0f;
        y = std::asin(sy);

        // Gimbal lock: cos(y) ~ 0 leaves x and z degenerate, so fold into x.
        if (std::fabs(sy) > 0.99999f) {
            x = std::atan2(-m[6], m[5]);
            z = 0.0f;
        } else {
            x = std::atan2(m[9], m[10]);
            z = std::atan2(m[4], m[0]);
        }
        return true;
#else
        (void)x; (void)y; (void)z;
        return false;
#endif
    }

    // Rebuilds the 3x3 basis from Euler XYZ (radians), keeps the actor's
    // current translation, and publishes through setMtx (authoritative,
    // physics-aware - see kSetActorMtxAddr's own comment on why that
    // distinction matters and WarpTo/NudgeTo for the equivalent story on
    // position). Any scale baked into the old basis is dropped - the result
    // is a pure rotation.
    //
    // FIXED: this used to call SetMatrix (the weak, actor-side-only path)
    // despite this comment already claiming setMtx - the doc was aspirational
    // and the code never matched it. On any actor physics drives (the player,
    // certainly), updateMtxFromPhysics silently reverted the write on the
    // very next frame, before it was ever rendered: SetRotation reported
    // success every call and produced no visible rotation at all. Caught by
    // Actor::SpinBy logging "success" every tick with nothing turning on
    // screen.
    //
    // Returns false for a map placement, for the same reason SetPosition does.
    bool SetRotation(float x, float y, float z) const {
#if !WIIXL_SWITCH
        if (!m_Ptr || !IsReadablePtr(m_Ptr) || m_Kind == Kind::Placement) return false;

        float m[12];
        if (!GetMatrix(m)) return false;

        const float sx = std::sin(x), cx = std::cos(x);
        const float sy = std::sin(y), cy = std::cos(y);
        const float sz = std::sin(z), cz = std::cos(z);

        m[0] = cy * cz;                 // m[0][0]
        m[1] = sx * sy * cz - cx * sz;  // m[0][1]
        m[2] = cx * sy * cz + sx * sz;  // m[0][2]
        m[4] = cy * sz;                 // m[1][0]
        m[5] = sx * sy * sz + cx * cz;  // m[1][1]
        m[6] = cx * sy * sz - sx * cz;  // m[1][2]
        m[8] = -sy;                     // m[2][0]
        m[9] = sx * cy;                 // m[2][1]
        m[10] = cx * cy;                // m[2][2]
        // m[3], m[7], m[11] (translation) left as read.

        return SetMtx(m, true, true);
#else
        (void)x; (void)y; (void)z;
        return false;
#endif
    }

    Vec3 GetRotation() const {
        Vec3 rot;
        GetRotation(rot.x, rot.y, rot.z);
        return rot;
    }

    bool SetRotation(const Vec3& rot) const {
        return SetRotation(rot.x, rot.y, rot.z);
    }

    // A software spin, not an engine-level angular velocity - built on the
    // already-confirmed SetRotation/setMtx path rather than a raw memory
    // field, because no genuine "angular velocity" input was ever found that
    // an active actor actually accepts:
    //
    //   * Actor::setVelocity's angular parameter (actor+0x268, see the
    //     comment on kControllerVelocityOffset's SetControllerVelocity)
    //     shares SetLinearVelocity's state==0||2 guard - the same branch,
    //     in the same function - so it is refused for the player exactly
    //     the way linear velocity is, with the same certainty.
    //   * The one controller-level field found by tracing the engine's own
    //     "RotSpeed" movement code, controller+0x34 (see
    //     Actor::SetControllerDirection), is a STEERING direction for an
    //     actor already moving under speed, not an independent spin. Writing
    //     it alone to the player every frame via Player::OnTick - the
    //     confirmed-correct write timing - measured zero yaw change over
    //     three separate one-second runs while standing still.
    //
    // So: this reads the current rotation, adds a fixed per-call delta, and
    // republishes. Call it once per tick with a fixed increment (radians);
    // there is no authoritative per-tick delta-time available here to
    // convert a rate into one automatically, so the caller supplies
    // degreesPerSecond * (pi/180) * theirOwnTickDelta (or just a constant,
    // for a fixed-rate spin under a fixed frame rate).
    //
    // Deliberately NOT built on SetRotation, for the same reason NudgeTo is
    // not built on WarpTo. SetRotation calls SetMtx with refreshPhysics=true
    // - right for a one-shot snap, wrong for a value called every frame: that
    // flag resets the physics instance set (cloth, ragdoll, the character
    // controller's own transient state) on every call, and NudgeTo's own
    // comment already documents what continuous resetting does - a
    // WarpTo-based position correction applied every frame tore down and
    // rebuilt a SWIMMING player's state each frame and dragged him to the
    // river bed. SpinBy is the rotational analogue of that same continuous
    // case. MEASURED: with refreshPhysics=true (i.e. going through
    // SetRotation), the player span up to some angle and then jittered in
    // place rather than turning smoothly - the per-frame physics-state reset
    // fighting the per-frame rotation write. Building the matrix here and
    // publishing with refreshPhysics=false, exactly as NudgeTo does for
    // position, is what actually turns smoothly.
    bool SpinBy(float yawDeltaRad, float pitchDeltaRad = 0.0f, float rollDeltaRad = 0.0f) const {
#if !WIIXL_SWITCH
        float m[12];
        if (!GetMatrix(m)) return false;

        // Current Euler angles, decomposed the same way GetRotation does.
        float curX, curY, curZ;
        float sy = -m[8];
        if (sy > 1.0f) sy = 1.0f;
        if (sy < -1.0f) sy = -1.0f;
        curY = std::asin(sy);
        if (std::fabs(sy) > 0.99999f) {
            curX = std::atan2(-m[6], m[5]);
            curZ = 0.0f;
        } else {
            curX = std::atan2(m[9], m[10]);
            curZ = std::atan2(m[4], m[0]);
        }

        const float x = curX + pitchDeltaRad;
        const float y = curY + yawDeltaRad;
        const float z = curZ + rollDeltaRad;

        const float sx = std::sin(x), cx = std::cos(x);
        const float sy2 = std::sin(y), cy = std::cos(y);
        const float sz = std::sin(z), cz = std::cos(z);

        m[0] = cy * cz;
        m[1] = sx * sy2 * cz - cx * sz;
        m[2] = cx * sy2 * cz + sx * sz;
        m[4] = cy * sz;
        m[5] = sx * sy2 * sz + cx * cz;
        m[6] = cx * sy2 * sz - sx * cz;
        m[8] = -sy2;
        m[9] = sx * cy;
        m[10] = cx * cy;
        // m[3], m[7], m[11] (translation) left as read.

        return SetMtx(m, true, false);
#else
        (void)yawDeltaRad; (void)pitchDeltaRad; (void)rollDeltaRad;
        return false;
#endif
    }

    // --- World Query & Iteration ---
    static constexpr bool SupportsWorldQuery = !WIIXL_SWITCH;

    // Zero-allocation visitor over every live BaseProc the game itself knows
    // about.
    //
    // This used to walk a supposed red-black tree at BaseProcMgr+0x6C plus the
    // 256-entry BaseProcUnit pool at 0x105597C0, filtered by IsValidActorName.
    // Neither container is what it was taken for, so in practice only the
    // player ever came out of it. The real one, read off
    // BaseProcMgr::processJobs (0x0378ec20):
    //
    //   mgr+0x64  s32   job-type count      } sead::Buffer<JobTypeBucket>
    //   mgr+0x68  ptr   job-type buckets    }
    //
    //   bucket    0xC0 bytes = 8 priority slots x 0x18
    //   slot      0x18 bytes = 2 lists x 0xC
    //   list      +0x00 prev, +0x04 next/first, +0x08 s32 count
    //
    //   node      +0x04 next, +0x08 BaseProc*, +0x10 priority byte
    //
    // The 0xC0/0x18/0xC nesting is fixed by the iterators the job loop uses:
    // 0x03791fcc strides the bucket by 0x18 up to +0xC0, 0x03791f8c strides
    // that by 0xC and reads {next, count} at +0x04/+0x08, and 0x0379201c
    // advances through node+0x04, stopping when it reaches the list head - the
    // head being the list struct itself, which is what terminates the walk
    // below. node+0x08 is the proc because the job loop dispatches on exactly
    // that: FUN_0378b60c(*(node + 8), jobType).
    //
    // A proc registered for several job types appears in several buckets, so
    // visits are de-duplicated - without that, a caller nudging every actor by
    // a fixed step would move some of them several times per frame.
    //
    // Names are deliberately not filtered here. These pointers come from the
    // manager's own lists, so they are live procs by construction; requiring
    // GetName() to look like an actor name only threw real actors away.
    template <typename CallbackFn>
    static void ForEachDynamic(CallbackFn&& callback) {
#if !WIIXL_SWITCH
        uint8_t* mgr = *reinterpret_cast<uint8_t**>(impl::kBaseProcMgrAddr);
        if (!mgr || !IsReadablePtr(mgr)) return;

        // Held for the whole traversal, callback included. Three job threads
        // mutate these lists, and this hook itself runs on more than one of
        // them, so the lock is what keeps a walk from following a `next` that
        // another core just freed - and what makes the shared visit set below
        // safe without a second lock of our own.
        impl::ProcMgrLock lock(mgr + impl::kBaseProcMgrLockOffset);

        impl::ProcVisitSet& seen = impl::ProcVisitSetRef();
        const bool outermost = !impl::InProcTraversal();
        if (outermost) {
            seen.Clear();
            impl::InProcTraversal() = true;
        }
        struct TraversalScope {
            bool outermost;
            ~TraversalScope() { if (outermost) impl::InProcTraversal() = false; }
        } traversalScope{outermost};

        // Returns false when the callback asked to stop.
        auto visit = [&](void* proc) -> bool {
            if (!IsPlausibleProc(proc)) return true;
            if (!seen.Add(proc)) return true;
            Actor actor(proc, Kind::Proc);
            if constexpr (std::is_invocable_r_v<bool, CallbackFn, const Actor&>) {
                return callback(actor);
            } else {
                callback(actor);
                return true;
            }
        };

        uint32_t jobTypeCount = *reinterpret_cast<uint32_t*>(mgr + 0x64);
        uint8_t* buckets = *reinterpret_cast<uint8_t**>(mgr + 0x68);

        // The game only ever indexes this buffer with a small job-type id;
        // the cap is a sanity bound on a field read out of live memory.
        if (buckets && IsReadablePtr(buckets) && jobTypeCount <= impl::kMaxJobTypes) {
            for (uint32_t jt = 0; jt < jobTypeCount; ++jt) {
                uint8_t* bucket = buckets + jt * impl::kJobBucketSize;

                for (uint32_t li = 0; li < impl::kListsPerBucket; ++li) {
                    uint8_t* list = bucket + li * impl::kProcListSize;
                    int32_t count = *reinterpret_cast<int32_t*>(list + 0x08);
                    if (count <= 0) continue;

                    uint8_t* node = *reinterpret_cast<uint8_t**>(list + 0x04);
                    // count bounds the walk so a torn list cannot spin.
                    for (int32_t n = 0; n < count; ++n) {
                        if (!node || node == list || !IsReadablePtr(node)) break;
                        void* proc = *reinterpret_cast<void**>(node + 0x08);
                        if (!visit(proc)) return;
                        node = *reinterpret_cast<uint8_t**>(node + 0x04);
                    }
                }
            }
        }

        // Player, in case it is not registered for any job type this frame.
        uint8_t** ppPlayerTracker = reinterpret_cast<uint8_t**>(0x10463f38);
        if (ppPlayerTracker && IsReadablePtr(*ppPlayerTracker)) {
            uint8_t* tracker = *ppPlayerTracker;
            void* linkMgr = *reinterpret_cast<void**>(tracker + 0x34);
            uint32_t procId = *reinterpret_cast<uint32_t*>(tracker + 0x38);
            uint8_t flag = *reinterpret_cast<uint8_t*>(tracker + 0x3c);

            if (linkMgr && IsReadablePtr(linkMgr)) {
                using GetProcFn = void* (*)(void* mgr, uint32_t id, uint8_t flag);
                auto getProc = WiiXLaunch::GetTargetFunction<GetProcFn>(0x0, 0x0378d8dc);
                visit(getProc(linkMgr, procId, flag));
            }
        }
#else
        (void)callback;
#endif
    }

    // Zero-allocation visitor over every STATIC map placement object.
    template <typename CallbackFn>
    static void ForEachStatic(CallbackFn&& callback) {
#if !WIIXL_SWITCH
        uint8_t** pPlacementMgr = reinterpret_cast<uint8_t**>(0x1047c318);
        if (pPlacementMgr && *pPlacementMgr && IsReadablePtr(*pPlacementMgr)) {
            uint8_t* pPlacement = *pPlacementMgr;
            uint8_t* pMapPlacement = *reinterpret_cast<uint8_t**>(pPlacement + 0x11c);
            if (pMapPlacement && IsReadablePtr(pMapPlacement)) {
                // 6000 x 0x124 is the array's real fixed capacity, not a guess.
                // Every placement lookup in the game inlines the same
                // sead::SafeArray-style clamp against that exact literal:
                //
                //   base = placementMgr[0x11c] + 0x2D0;
                //   if (mapObject->idx16 < 6000) base += idx16 * 0x124;
                //
                // seen at 0x0379e3d4 (five times), 0x0379f2a0, 0x0313b84c,
                // 0x0313a878 and 0x0313a8e8 - the last writing it as
                // `(uint*)base + idx * 0x49`, which independently confirms the
                // 0x124 stride. So the array spans 0x2D0..0x6D050 and this walk
                // stays inside it; it is not an over-read.
                //
                // What there is no field for is how many slots the *current*
                // region actually populated. The game never iterates this array
                // - it only random-accesses it by the uint16 index each map
                // object carries at +0x04 - so no live count is reachable from
                // any of those paths. Stale slots therefore still hold whatever
                // the last region left there, which is what the name check
                // below is for. Do not drop it.
                uint8_t* entriesBase = pMapPlacement + 0x2d0;
                constexpr size_t kPlacementCount = 6000;
                constexpr size_t kPlacementEntrySize = 0x124;

                for (size_t i = 0; i < kPlacementCount; ++i) {
                    uint8_t* entry = entriesBase + (i * kPlacementEntrySize);
                    if (!IsReadablePtr(entry)) break;

                    const char* const* ppName = reinterpret_cast<const char* const*>(entry + 0xd8);
                    const char* name = nullptr;
                    if (IsReadablePtr(ppName)) name = *ppName;
                    if (!name || !IsReadablePtr(name)) name = reinterpret_cast<const char*>(entry + 0xe4);
                    if (!IsValidActorName(name)) continue;

                    Actor placementActor(entry, Kind::Placement);
                    if constexpr (std::is_invocable_r_v<bool, CallbackFn, const Actor&>) {
                        if (!callback(placementActor)) return;
                    } else {
                        callback(placementActor);
                    }
                }
            }
        }
#else
        (void)callback;
#endif
    }

    // Zero-allocation visitor over EVERY actor (both Dynamic and Static Map Placements).
    template <typename CallbackFn>
    static void ForEach(CallbackFn&& callback) {
        ForEachDynamic(callback);
        ForEachStatic(callback);
    }

    // Returns the total count of valid active actors currently loaded in the world.
    static size_t GetCount() {
        size_t count = 0;
        ForEach([&count](const Actor&) {
            ++count;
        });
        return count;
    }

    // Looks up an actor by its actor name / ID string (e.g. "Enemy_Bokoblin", "Player", etc.).
    static Actor GetActor(const char* name, bool exactMatch = false) {
        if (!name || name[0] == '\0') return Actor(nullptr);
        Actor found(nullptr);
        ForEach([&](const Actor& actor) {
            const char* actorName = actor.GetName();
            if (exactMatch) {
                if (std::strcmp(actorName, name) == 0) {
                    found = actor;
                    return false;
                }
            } else {
                if (std::strstr(actorName, name) != nullptr) {
                    found = actor;
                    return false;
                }
            }
            return true;
        });
        return found;
    }

    // Overload: looks up an actor by its unique numeric BaseProc ID.
    static Actor GetActor(uint32_t id) {
        Actor found(nullptr);
        ForEach([&](const Actor& actor) {
            if (actor.GetId() == id) {
                found = actor;
                return false;
            }
            return true;
        });
        return found;
    }

    // Backwards-compatible alias for GetActor(id)
    static Actor GetById(uint32_t id) { return GetActor(id); }

    // Backwards-compatible alias for GetActor(name)
    static Actor FindByName(const char* name, bool exactMatch = false) { return GetActor(name, exactMatch); }
    static Actor Get(const char* name, bool exactMatch = false) { return GetActor(name, exactMatch); }

    // Queries only DYNAMIC active BaseProc actors.
    static std::vector<Actor> QueryDynamicActors(const char* name = nullptr, bool exactMatch = false) {
        std::vector<Actor> actors;
        ForEachDynamic([&](const Actor& actor) {
            if (name == nullptr || name[0] == '\0') {
                actors.push_back(actor);
            } else {
                const char* actorName = actor.GetName();
                if (exactMatch) {
                    if (std::strcmp(actorName, name) == 0) actors.push_back(actor);
                } else {
                    if (std::strstr(actorName, name) != nullptr) actors.push_back(actor);
                }
            }
        });
        return actors;
    }

    // Alias for QueryDynamicActors
    static std::vector<Actor> GetDynamicActors(const char* name = nullptr, bool exactMatch = false) {
        return QueryDynamicActors(name, exactMatch);
    }

    // Queries only STATIC map placement objects.
    static std::vector<Actor> QueryStaticActors(const char* name = nullptr, bool exactMatch = false) {
        std::vector<Actor> actors;
        ForEachStatic([&](const Actor& actor) {
            if (name == nullptr || name[0] == '\0') {
                actors.push_back(actor);
            } else {
                const char* actorName = actor.GetName();
                if (exactMatch) {
                    if (std::strcmp(actorName, name) == 0) actors.push_back(actor);
                } else {
                    if (std::strstr(actorName, name) != nullptr) actors.push_back(actor);
                }
            }
        });
        return actors;
    }

    // Alias for QueryStaticActors
    static std::vector<Actor> GetStaticActors(const char* name = nullptr, bool exactMatch = false) {
        return QueryStaticActors(name, exactMatch);
    }

    // Queries ALL actors currently loaded in the world (both Dynamic and Static).
    static std::vector<Actor> GetAllActors(const char* name = nullptr, bool exactMatch = false) {
        std::vector<Actor> actors;
        actors.reserve(64);
        ForEach([&](const Actor& actor) {
            if (name == nullptr || name[0] == '\0') {
                actors.push_back(actor);
            } else {
                const char* actorName = actor.GetName();
                if (exactMatch) {
                    if (std::strcmp(actorName, name) == 0) {
                        actors.push_back(actor);
                    }
                } else {
                    if (std::strstr(actorName, name) != nullptr) {
                        actors.push_back(actor);
                    }
                }
            }
        });
        return actors;
    }

    // Backwards-compatible alias for GetAllActors
    static std::vector<Actor> GetAll(const char* name = nullptr, bool exactMatch = false) {
        return GetAllActors(name, exactMatch);
    }

private:
    // Whether a pointer read off an actor is worth dereferencing. Matches the
    // range checks the Life accessors above already use for the same reason:
    // these are fields whose layout is inferred, so a wrong guess has to fail
    // as a null read rather than as a crash. (Unguarded because GetName /
    // IsValidActorName compile on every platform; the address band is
    // per-platform: Wii U MEM2 vs the Switch 39-bit address space.)
    static bool IsReadablePtr(const void* p) {
        uintptr_t v = reinterpret_cast<uintptr_t>(p);
#if WIIXL_SWITCH
        return v >= 0x8000000 && v < (uintptr_t(1) << 40);
#else
        return v >= 0x10000000 && v < 0xa0000000;
#endif
    }

#if !WIIXL_SWITCH

    // Whether a pointer pulled out of a BaseProcMgr job list is really a
    // BaseProc. IsReadablePtr alone is far too weak: it passed 0x62caf01e, a
    // misaligned pointer into a region that holds no actors, which then took a
    // 48-byte matrix write at +0x1F8 and +0x22C from SetPosition.
    //
    // Two checks, both grounded rather than guessed:
    //
    //  - Alignment. A BaseProc holds pointers and floats, so it is at least
    //    4-aligned; every one of the 522 good procs in that same sweep ended
    //    in 0, 4, 8 or C, and the bad one ended in E.
    //  - A vtable at +0xE8. Every BaseProc has one and the game dereferences
    //    it without checking - 0x0378a374 calls slot 0x5C through it and
    //    0x0378b60c calls slot 0xCC - so its absence means this is not a proc.
    //    The accepted band matches what the Life accessors above already use.
    static bool IsPlausibleProc(const void* p) {
        uintptr_t v = reinterpret_cast<uintptr_t>(p);
        if (v == 0 || (v & 3) != 0 || !IsReadablePtr(p)) return false;

        uintptr_t vtable = *reinterpret_cast<const uintptr_t*>(
            static_cast<const uint8_t*>(p) + 0xe8);
        if ((vtable & 3) != 0) return false;
        return vtable >= 0x02000000 && vtable <= 0x10600000;
    }

    // Bit test rather than std::isfinite, so this cannot be affected by the
    // FP mode the game happens to be running in: exponent all ones is NaN or
    // infinity, everything else is finite.
    static bool IsFiniteFloat(float f) {
        uint32_t bits;
        std::memcpy(&bits, &f, sizeof(bits));
        return (bits & 0x7f800000u) != 0x7f800000u;
    }

    // Whether a matrix is usable as a transform at all.
    //
    // Not every actor keeps a real one. BotW's logic-gate actors - LinkTagOr,
    // LinkTagAnd - carry NaN in theirs, presumably because nothing ever
    // initialises a transform they do not use. Reading that, adding to it and
    // handing it back to setMtx writes NaN into +0x1F8, into +0x22C, and
    // through the attach-frame path at 0x034d3d40, which is how a NaN gets
    // into the matrix pipeline the physics and render sides both consume.
    //
    // This doubles as the discriminator that a type check would have given:
    // an object that does not hold a finite transform is not one to move,
    // whatever its class turns out to be.
    static bool IsFiniteMatrix(const float m[12]) {
        for (uint32_t i = 0; i < 12; ++i) {
            if (!IsFiniteFloat(m[i])) return false;
        }
        return true;
    }
#endif

    void* m_Ptr;
    Kind m_Kind = Kind::Proc;
};

} // namespace WiiXLaunch::BotW
