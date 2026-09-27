#pragma once

// Spawn queue for the host: queue any number of actors (optionally armed, see
// SpawnLoadout in wiixlaunch/botw/game/actor.hpp) and they go out one per
// tick, in the order they were added.
//
// Usage:
//   AddPendingSpawn("Enemy_Bokoblin_Junior", x, y, z);
//   AddPendingSpawn("Enemy_Bokoblin_Junior", x, y, z,
//                   {.weapon = "Weapon_Sword_001", .shield = "Weapon_Shield_001"});
//   AddPendingSpawn("Enemy_Bokoblin_Junior", x, y, z,
//                   {.bow = "Weapon_Bow_001", .arrow = "FireArrow"});
//
// ExecutePendingSpawn() has to run once per tick, from a hook that runs every
// frame (main.cpp calls it from PlayerTickHook). Actor::Spawn itself only
// accepts one request at a time, so the queue is what makes adding many at
// once work.
//
// Header-only on purpose: the build scripts list their .cpp files by hand
// (build_cemu.sh, build_wiiu.sh, the .bat files), so a new .cpp would have to
// be added to every one of them. A header just needs the #include.

#include "../wiixlaunch.hpp"
#include "wiixlaunch/botw/botw.hpp"

namespace SpawnQueue {

using WiiXLaunch::BotW::Actor;
using WiiXLaunch::BotW::Player;
using WiiXLaunch::BotW::SpawnLoadout;

constexpr int kMaxPendingSpawns = 13;

// How many ticks an armed spawn waits for the previous enemy to pick up its
// equipment before going ahead anyway (see Actor::IsWeaponRequestPending).
constexpr int kWeaponWaitTicks = 120;

// The strings in name and loadout are stored as pointers, not copied, so
// they must outlive the queue entry - string literals do. Actor::Spawn copies
// them once the entry actually goes out.
struct PendingSpawn {
    const char* name = "noname";
    SpawnLoadout loadout;   // optional equipment, empty = unarmed
    float x = 0, y = 0, z = 0;
    bool active = false;

    bool IsArmed() const {
        auto set = [](const char* s) { return s && s[0]; };
        if (set(loadout.weapon) || set(loadout.shield) || set(loadout.bow) || set(loadout.arrow))
            return true;
        for (const char* s : loadout.slot)
            if (set(s)) return true;
        return false;
    }
};

// FIFO ring: spawns go out in the order they were added.
struct State {
    PendingSpawn entries[kMaxPendingSpawns];
    int head = 0;          // next entry to spawn
    int count = 0;
    int weaponWait = 0;
    void (*onAdded)() = nullptr;
};

inline State& Get() {
    static State s;
    return s;
}

}  // namespace SpawnQueue

// Optional: called once for every spawn AddPendingSpawn accepts - e.g. to
// count enemies for a wave system. Only accepted spawns count; a spawn
// refused because the queue is full never happens, so it is not reported.
inline void SetOnPendingSpawnAdded(void (*callback)()) {
    SpawnQueue::Get().onAdded = callback;
}

// Queues an actor with optional equipment. Returns false when the queue is
// full (kMaxPendingSpawns entries waiting); nothing is queued then.
inline bool AddPendingSpawn(const char* name, float x, float y, float z,
                            const SpawnQueue::SpawnLoadout& loadout) {
    auto& q = SpawnQueue::Get();
    if (q.count >= SpawnQueue::kMaxPendingSpawns) return false;

    SpawnQueue::PendingSpawn& slot = q.entries[(q.head + q.count) % SpawnQueue::kMaxPendingSpawns];
    slot.name = name;
    slot.loadout = loadout;
    slot.x = x;
    slot.y = y;
    slot.z = z;
    slot.active = true;
    q.count++;

    if (q.onAdded) q.onAdded();
    return true;
}

// Short form: an optional melee weapon only.
inline bool AddPendingSpawn(const char* name, float x, float y, float z,
                            const char* weapon = nullptr) {
    SpawnQueue::SpawnLoadout loadout;
    loadout.weapon = weapon;
    return AddPendingSpawn(name, x, y, z, loadout);
}

// Sends the next queued spawn out, at most one per call. Call once per tick.
inline void ExecutePendingSpawn() {
    using namespace SpawnQueue;
    auto& q = Get();
    if (q.count == 0) return;

    PendingSpawn& next = q.entries[q.head];

    // Only one armed request is held at a time and a new one replaces it, so
    // wait until the previous enemy has taken its equipment.
    if (next.IsArmed() && Actor::IsWeaponRequestPending() && q.weaponWait < kWeaponWaitTicks) {
        q.weaponWait++;
        return;
    }

    Actor player(Player::GetRaw());
    // false = the framework's slot is still busy; keep the entry and retry
    // next tick instead of dropping it.
    if (!Actor::Spawn(next.name, player, next.x, next.y, next.z, next.loadout)) return;

    next.active = false;
    q.head = (q.head + 1) % kMaxPendingSpawns;
    q.count--;
    q.weaponWait = 0;
}
