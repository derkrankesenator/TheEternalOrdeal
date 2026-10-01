#pragma once

// Ordeal: spawning enemies WITH equipment - melee weapon, shield, bow and
// arrow type - on top of the botw module's Actor::Spawn.
//
//   Ordeal::InitEquip();   // once, in WiiXLaunch_Init(), after Actor::Init()
//
//   Ordeal::Spawn("Enemy_Bokoblin_Junior", anchor, x, y, z, "Weapon_Sword_001");
//   Ordeal::Spawn("Enemy_Bokoblin_Junior", anchor, x, y, z,
//                 {.bow = "Weapon_Bow_001", .arrow = "FireArrow"});
//   Ordeal::Spawn("Enemy_Lynel_Gold", anchor, x, y, z,
//                 {.weapon = "Weapon_Sword_033", .shield = "Weapon_Shield_033",
//                  .bow = "Weapon_Bow_033", .arrow = "FireArrow"});
//
// (pending_spawn.hpp's AddPendingSpawn wraps this with a queue.)
//
// WHY A SEPARATE FILE. This used to be patched into the botw module's
// wiixlaunch/botw/game/actor.hpp. That module is vendored from upstream
// (vendor/wiixlaunch-botw), so changes there are lost or conflict on every
// update. Everything here only USES actor.hpp - Actor::Spawn, Actor::Init,
// the hook macros - and adds two hooks of its own; actor.hpp is back to the
// upstream file (commit 49e07a6, plus the project's one-line change of
// ExecuteSpawn's pack from `static` to a local).
//
// Wii U / Cemu only, like Actor::Spawn itself. On Switch every call below is
// a no-op that returns false, and InitEquip installs nothing.

#include "wiixlaunch/botw/botw.hpp"

#include <cstdint>
#include <cstring>

namespace Ordeal {

// What Ordeal::Spawn / Ordeal::SpawnScaled should equip a spawned enemy with.
//
// Every field is optional; a default-constructed SpawnLoadout spawns exactly
// what Actor::Spawn spawns. The strings are copied when the spawn is queued,
// so they only have to live until Spawn returns.
//
//   weapon  melee weapon - "Weapon_Sword_001", "Weapon_Spear_001",
//           "Weapon_Lsword_001" ... Goes into slot 0 (EquipItem1), the hand.
//   shield  "Weapon_Shield_001" ... Slot 1 (EquipItem2).
//   bow     "Weapon_Bow_001" ... Slot 0 when there is no weapon - the way a
//           bow Bokoblin/Moriblin/Lizalfos is placed in the map - otherwise
//           slot 2 (EquipItem3), the way a Lynel is. A Bokoblin given BOTH
//           holds the sword; its slot 2 is its horn slot, so the bow is
//           attached but no arrow AI uses it. That is how these enemies are
//           built, not a limit of the hook - see WHICH SLOT IS WHAT below.
//   arrow   arrow actor for the bow, as a map placement's ArrowName:
//           "NormalArrow", "FireArrow", "IceArrow", "ElectricArrow",
//           "BombArrow_A", "AncientArrow". Answered for the enemy's whole life,
//           not only at init - see ARROWS below.
//   slot[]  raw per-slot names, EquipItem<i+1>, overriding the named fields
//           above. For layouts the three named fields do not fit: a quiver
//           ("BokoblinPipe" / "LizalfosPipe" in slot 2, as the map puts next
//           to a bow), a Hinox (EquipItem3..5), or anything else.
//
// Every name must be an existing actor. A name that is not - a typo such as
// "Weapon_Shield:033" - makes the game's createEquipments abort at that slot,
// and the enemy comes up with none of its equipment.
//
// Only an Enemy reads any of this; on other actors it has no effect. The game
// may still upgrade a weapon through the level sensor, exactly as it does for
// a placed enemy.
struct SpawnLoadout {
    const char* weapon = nullptr;
    const char* shield = nullptr;
    const char* bow = nullptr;
    const char* arrow = nullptr;
    const char* slot[6] = {};
};

namespace detail {

#if !WIIXL_SWITCH

constexpr uint32_t kEnemyEquipSlotCount = 6;   // Enemy::createEquipments loops 0..5

// Copies at most cap-1 bytes and always terminates. (Its own copy rather than
// actor.hpp's impl::CopyActorName, so nothing here depends on that module's
// internals.)
inline void CopyName(char* dest, uint32_t cap, const char* src) {
    uint32_t i = 0;
    if (src) {
        for (; i + 1 < cap && src[i]; ++i) dest[i] = src[i];
    }
    dest[i] = '\0';
}

// What one spawn should be equipped with, as copied strings ready to answer
// the two getters (Enemy::getEquippedItem and Enemy::getArrowName - see the
// "Spawning an enemy with equipment" block below for both). Filled from a
// SpawnLoadout by BuildEquipRequest. Copied rather than pointed at, because
// the request outlives the call that made it.
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

// --- Spawning an enemy with equipment (Wii U V208) -------------------------
//
// Covers Ordeal::Spawn(..., weapon) and Ordeal::Spawn(..., SpawnLoadout):
// melee weapon, shield, bow and arrow type. The weapons go through
// getEquippedItem (this block); the arrow type goes through a second getter,
// see ARROWS further down.
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
// "on failure ... return 0" is why one bad name costs ALL the equipment: the
// loop does not skip the slot, it stops.
//
// The actor-create manager at 0x1047c2b8 and the heap at *(0x10463f6c)+0x10
// are the same two globals actor.hpp's impl::ExecuteSpawn uses
// (kActorCreateMgrAddr, kHeapProviderAddr). That is independent confirmation
// that ExecuteSpawn picked the game's normal creation path.
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
// WHY NOT THROUGH THE PACK. The InstParamPack ExecuteSpawn builds cannot carry
// this. InstParamPack knows "@P", "@R", "@M", "@S", "@D", "@DD", "@TV",
// "@RV", "@MU", "@RL", "@W", "@PC", "@ND", "@DC", "@SB" and "@MA" (all in
// ksys::act::InstParamPack::Buffer in the decompilation). None of them is
// equipment, and getEquippedItem never reads the pack.
//
// THE FIX. EnemyGetEquippedItemHook answers getEquippedItem for the enemy we
// spawned and passes every other call through. Everything after that - the
// weapon actor, the slot link, the +0xa09 bit, equipWeapon, the hold flag the
// AI's WeaponIdx param reads - is the game's own code running as it does for
// a placed enemy. Nothing is written into the enemy by hand. The arrow type
// works the same way through a second getter (EnemyGetArrowNameHook, see
// ARROWS). Hooking the getter rather than createEquipments keeps the patch to
// one answer, and it also covers the one override found: 0x02482838 (an
// Enemy subclass that handles tag 0x12ad8456 itself, otherwise tail-calls
// createEquipments) runs its own slot loop through the same vtable +0x57c.
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
//     bytes with the function at +4 (see the Life accessors in actor.hpp), so
//     index N sits at N*8 + 4. Switch entries are plain 8-byte pointers at
//     N*8. 175, 98 and 164 come out identical on both platforms.
// The same file's pairing of ActorWeapons::getEquippedWeapon (0x7100efbc44 ->
// 0x033bccd8, score 0.46, 2 vs 14 parameters) is wrong and was not used.
//
// Both hook targets start with a plain prologue in the installed v208 RPX
// (stwu / mflr / stmw / mr - no PC-relative branch in the first four
// instructions), so the hook manager accepts them.
//
// For reference, what equipWeapon fills in, as read in Switch 1.5.0 only (the
// Wii U layout was not traced): PlayerOrEnemy::getWeapons returns this+0xb90,
// an ActorWeapons with 6 slots of 0x18 bytes starting at +0x08, each
// {BaseProcLink weapon; bool sheathed @+0x10}, owner at +0x98. This is what
// the AI's WeaponIdx param indexes.
//
// WHEN THE REQUEST IS ARMED. Ordeal::Spawn arms it right after Actor::Spawn
// has accepted the spawn - Actor::Spawn only queues, and SpawnFlushHook
// creates the actor on a later actor tick, so the request is always in place
// before the enemy's init can ask. (When this lived inside actor.hpp it was
// armed in ExecuteSpawn, just before spawnActor, i.e. one flush later. Arming
// from outside actor.hpp moves it earlier by that one flush and changes
// nothing else.)
//
// MATCHING OUR ENEMY. Actor::Spawn cannot hand the created actor back, so
// there is no pointer to compare against. The request is keyed by actor NAME
// instead. The first actor of that name to call the getter claims it by
// pointer, answers for all its slots, and the request ends once the claimant
// has asked for its last slot (5). The name is read from
// *(char**)(actor+0x04), the same field createEquipments compares against
// "Enemy_Guardian_Mini" and Actor::GetName reads on Wii U.
//
// CONFIRMED LIVE on Cemu: an Enemy_Lynel_Gold spawned with sword, shield and
// bow came up holding all three. Not separately confirmed: the arrow type
// (see ARROWS) and the humanoid bow-only layout. Statically:
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
//   * A game-triggered spawn of the same actor name in the window between our
//     spawn and our enemy's init can claim the request first. Accepted, for
//     want of anything better to key on.
//   * The getter runs on the thread initialising the enemy, and the request
//     is written from whichever thread calls Ordeal::Spawn. There is no lock:
//     ArmSpawnEquip clears `valid` first and sets it last, so a reader sees
//     either no request or a complete one. The fields are volatile only to
//     stop the compiler caching them across the two threads.
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
//   Enemy_Lynel_Gold / Lynel_Junior
//     Grab:  Slot0Node Weapon_R, Slot1Node Weapon_L, Slot2Node Weapon_R,
//            Slot5Node Head, Slot0/1/2PodNode Pod_B / Pod_C / Pod_A
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
//   Lynel: EquipItem1 melee, EquipItem2 Shield, EquipItem3 Bow - e.g.
//          Enemy_Lynel_Gold with Weapon_Sword_047 / Weapon_Shield_033 /
//          Weapon_Bow_033 and ArrowName ElectricArrow
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
// NOT MEASURED: which of those callers actually decides the arrow actor a
// bow shoots was not traced. If the arrow does not change, that is where to
// look.
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
        if (name && name[0]) CopyName(out.items[slot], sizeof(out.items[slot]), name);
    };
    set(kSlotMainHand, in.weapon);
    set(kSlotShield, in.shield);
    if (in.bow && in.bow[0]) set((in.weapon && in.weapon[0]) ? kSlotSecond : kSlotMainHand, in.bow);
    for (uint32_t i = 0; i < kEnemyEquipSlotCount; ++i) set(i, in.slot[i]);
    if (in.arrow && in.arrow[0]) CopyName(out.arrow, sizeof(out.arrow), in.arrow);
}

// The one armed request. There is one because Actor::Spawn holds one spawn,
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
    CopyName(o.actorName, sizeof(o.actorName), actorName);
    CopyName(o.arrow, sizeof(o.arrow), arrow);
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

#endif  // !WIIXL_SWITCH

// Counters for finding out where an equipped spawn goes wrong, read through
// Ordeal::GetEquipDebug(). Nothing in the path above can report an error back
// to the caller - the getter is called by the game, from the enemy's init,
// often on another thread - so these are the only way to tell "the hook never
// ran", "it ran but never matched our enemy" and "it answered, and the game
// still did not create the weapon" apart in a live session.
//
// Plain counters, written without a lock from whichever thread runs the hook.
// A torn read shows up as one off-by-one frame in a log and nothing worse.
// Defined on every platform so callers can log them unconditionally; on
// Switch they simply stay 0.
struct EquipDebug {
    volatile uint32_t arms = 0;             // ArmSpawnEquip calls that armed a request
    volatile uint32_t getterCalls = 0;      // every getEquippedItem call, any actor
    volatile uint32_t getterWhileArmed = 0; // ...of those, while a request was armed
    volatile uint32_t claims = 0;           // requests claimed by a matching enemy
    volatile uint32_t slotsAnswered = 0;    // slots answered with one of our names
    volatile uint32_t arrowCalls = 0;       // every getArrowName call, any actor
    volatile uint32_t arrowsAnswered = 0;   // ...of those answered from ArrowOverrideTable
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

#if !WIIXL_SWITCH

// Arms the request for the actor Ordeal::Spawn just queued - see WHEN THE
// REQUEST IS ARMED.
//
// A spawn WITHOUT equipment leaves an earlier armed request alone rather than
// clearing it, because the enemy that request belongs to may still be
// initialising. A new ARMED spawn replaces it. Callers queueing several armed
// enemies wait on Ordeal::IsEquipRequestPending between them.
//
// An arrow-only request is armed too: the arrow reaches ArrowOverrideTable
// through the claim in EnemyGetEquippedItemHook, which every enemy passes
// through during init whether or not it gets a weapon.
inline void ArmSpawnEquip(const char* actorName, const EquipRequest& req) {
    if (!req.Any()) return;
    EquipDebugRef().arms = EquipDebugRef().arms + 1;
    SpawnEquip& e = SpawnEquipRef();
    e.valid = false;
    e.claimedBy = nullptr;
    CopyName(e.actorName, sizeof(e.actorName), actorName);
    e.req = req;
    e.valid = true;
}

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
// IsEquipRequestPending wait in callers already rules out.)
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
                CopyName(dbg.lastAskedWhileArmed, sizeof(dbg.lastAskedWhileArmed),
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
                CopyName(buffer, static_cast<uint32_t>(cap), o.arrow);
                dbg.arrowsAnswered = dbg.arrowsAnswered + 1;
                return 1;
            }
        }
        return Orig(enemy, outString);
    }
};

#endif  // !WIIXL_SWITCH

}  // namespace detail

// Installs the two equipment hooks. Call once from WiiXLaunch_Init(), after
// WiiXLaunch::BotW::Actor::Init() (which installs the spawn plumbing these
// build on). Both hooks pass every call through untouched while no armed
// request exists, so installing them costs nothing for the rest of the game.
inline void InitEquip() {
#if !WIIXL_SWITCH
    detail::EnemyGetEquippedItemHook::Install(0x0, detail::kEnemyGetEquippedItemAddr);
    detail::EnemyGetArrowNameHook::Install(0x0, detail::kEnemyGetArrowNameAddr);
#endif
}

// Actor::Spawn with equipment. Returns what Actor::Spawn returns: true means
// the spawn was queued (and its equipment armed), false that Actor::Spawn's
// single slot is still busy - retry next tick, nothing was armed.
//
// Equipment is not written into the creation params, since no param exists
// for it. It arms detail::EnemyGetEquippedItemHook, which answers the enemy's
// own weapon query during its init, and, for an arrow,
// detail::EnemyGetArrowNameHook, which answers the arrow query for the
// enemy's whole life. The game creates and equips the weapons itself.
//
// Things to know:
//   * only an Enemy asks, so on anything else it has no effect;
//   * weapons may come out upgraded by the level sensor, as for a placed
//     enemy;
//   * one armed request at a time: a second armed Spawn before the first
//     enemy has initialised replaces the first request, and that enemy
//     comes up unarmed. Wait on IsEquipRequestPending() between armed
//     spawns (pending_spawn.hpp does).
inline bool Spawn(const char* actorName, const WiiXLaunch::BotW::Actor& anchor,
                  float x, float y, float z, const SpawnLoadout& loadout) {
#if WIIXL_SWITCH
    (void)actorName; (void)anchor; (void)x; (void)y; (void)z; (void)loadout;
    return false;
#else
    if (!WiiXLaunch::BotW::Actor::Spawn(actorName, anchor, x, y, z)) return false;
    detail::EquipRequest req;
    detail::BuildEquipRequest(req, loadout);
    detail::ArmSpawnEquip(actorName, req);
    return true;
#endif
}

// Short form: an optional melee weapon only.
inline bool Spawn(const char* actorName, const WiiXLaunch::BotW::Actor& anchor,
                  float x, float y, float z, const char* weapon = nullptr) {
    SpawnLoadout loadout;
    loadout.weapon = weapon;
    return Spawn(actorName, anchor, x, y, z, loadout);
}

// Actor::SpawnScaled with equipment - see Spawn above and Actor::SpawnScaled
// for the scale.
inline bool SpawnScaled(const char* actorName, const WiiXLaunch::BotW::Actor& anchor,
                        float x, float y, float z, float scaleX, float scaleY, float scaleZ,
                        const SpawnLoadout& loadout) {
#if WIIXL_SWITCH
    (void)actorName; (void)anchor; (void)x; (void)y; (void)z;
    (void)scaleX; (void)scaleY; (void)scaleZ; (void)loadout;
    return false;
#else
    if (!WiiXLaunch::BotW::Actor::SpawnScaled(actorName, anchor, x, y, z, scaleX, scaleY, scaleZ))
        return false;
    detail::EquipRequest req;
    detail::BuildEquipRequest(req, loadout);
    detail::ArmSpawnEquip(actorName, req);
    return true;
#endif
}

// Whether an armed spawn is still waiting for its enemy to ask for its
// equipment, i.e. detail::SpawnEquip is armed and its claimant has not yet
// reached slot 5. "Armed" means any equipment at all, an arrow-only
// SpawnLoadout included. Once the enemy has claimed the request, its arrow
// lives on in detail::ArrowOverrideTable and no longer counts here.
//
// Only one armed request is held at a time and a new armed spawn replaces it,
// so a caller spawning several armed enemies waits for this to clear between
// them. pending_spawn.hpp's ExecutePendingSpawn is the reference user.
//
// It can stay true for good: when the enemy never builds its equipment, or
// when the name never matched an enemy (a typo, or a non-enemy actor given a
// weapon). So a caller should give up after a while rather than wait
// forever; the next armed spawn simply replaces the stale request.
inline bool IsEquipRequestPending() {
#if WIIXL_SWITCH
    return false;
#else
    return detail::SpawnEquipRef().valid;
#endif
}

// Drops an armed request that no enemy has claimed yet - for a reload, when
// the enemy it was meant for will never initialise. Enemies that already
// claimed theirs keep their arrow (detail::ArrowOverrideTable); those entries
// check the actor name on every use and are harmless once the enemy is gone.
inline void CancelEquipRequest() {
#if !WIIXL_SWITCH
    detail::SpawnEquip& e = detail::SpawnEquipRef();
    e.valid = false;
    e.claimedBy = nullptr;
#endif
}

// Diagnostic counters of the equipment hooks - see detail::EquipDebug for
// what each one means. Read-only; log them from a tick to see how far an
// equipped spawn got:
//   arms == 0              the spawn never carried equipment
//   getterWhileArmed == 0  no enemy asked for equipment after arming
//   claims == 0            enemies asked, but lastAskedWhileArmed is not
//                          the name that was requested
//   slotsAnswered > 0      names were handed to the game; if the enemy is
//                          still unarmed, the game dropped them after that
//                          (typically one name that is not an actor)
inline const detail::EquipDebug& GetEquipDebug() { return detail::EquipDebugRef(); }

}  // namespace Ordeal
