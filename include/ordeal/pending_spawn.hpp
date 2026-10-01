#pragma once

#include "wiixlaunch/botw/botw.hpp"
#include "enemy_equip.hpp"

namespace SpawnQueue {

using WiiXLaunch::BotW::Actor;
using WiiXLaunch::BotW::Player;
using Ordeal::SpawnLoadout;

constexpr int kMaxPendingSpawns = 13;

constexpr int kWeaponWaitTicks = 120;

inline void CopyName(char* dest, int cap, const char* src) {
    int i = 0;
    if (src) {
        for (; i + 1 < cap && src[i]; ++i) dest[i] = src[i];
    }
    dest[i] = '\0';
}

struct PendingSpawn {
    static constexpr int kNameLen = 64;   // longest actor names are ~40 chars
    static constexpr int kSlots = 6;

    char name[kNameLen] = {};
    char weapon[kNameLen] = {};
    char shield[kNameLen] = {};
    char bow[kNameLen] = {};
    char arrow[kNameLen] = {};
    char slot[kSlots][kNameLen] = {};
    float x = 0, y = 0, z = 0;
    bool active = false;

    void Set(const char* actorName, float px, float py, float pz, const SpawnLoadout& l) {
        CopyName(name, kNameLen, actorName);
        CopyName(weapon, kNameLen, l.weapon);
        CopyName(shield, kNameLen, l.shield);
        CopyName(bow, kNameLen, l.bow);
        CopyName(arrow, kNameLen, l.arrow);
        for (int i = 0; i < kSlots; ++i) CopyName(slot[i], kNameLen, l.slot[i]);
        x = px;
        y = py;
        z = pz;
    }

    SpawnLoadout Loadout() const {
        auto opt = [](const char* s) -> const char* { return s[0] ? s : nullptr; };
        SpawnLoadout l;
        l.weapon = opt(weapon);
        l.shield = opt(shield);
        l.bow = opt(bow);
        l.arrow = opt(arrow);
        for (int i = 0; i < kSlots; ++i) l.slot[i] = opt(slot[i]);
        return l;
    }

    bool IsArmed() const {
        if (weapon[0] || shield[0] || bow[0] || arrow[0]) return true;
        for (int i = 0; i < kSlots; ++i)
            if (slot[i][0]) return true;
        return false;
    }
};

struct State {
    PendingSpawn entries[kMaxPendingSpawns];
    int head = 0;
    int count = 0;
    int weaponWait = 0;
    void (*onAdded)() = nullptr;
};

inline State& Get() {
    static State s;
    return s;
}

}

inline void SetOnPendingSpawnAdded(void (*callback)()) {
    SpawnQueue::Get().onAdded = callback;
}

inline bool AddPendingSpawn(const char* name, float x, float y, float z,
                            const SpawnQueue::SpawnLoadout& loadout) {
    auto& q = SpawnQueue::Get();
    if (q.count >= SpawnQueue::kMaxPendingSpawns) return false;

    SpawnQueue::PendingSpawn& slot = q.entries[(q.head + q.count) % SpawnQueue::kMaxPendingSpawns];
    slot.Set(name, x, y, z, loadout);
    slot.active = true;
    q.count++;

    if (q.onAdded) q.onAdded();
    return true;
}



inline int PendingSpawnCount() {
    return SpawnQueue::Get().count;
}

inline void ClearPendingSpawns() {
    auto& q = SpawnQueue::Get();
    for (auto& e : q.entries) e.active = false;
    q.head = 0;
    q.count = 0;
    q.weaponWait = 0;
    Ordeal::CancelEquipRequest();
}
inline void ExecutePendingSpawn() {
    using namespace SpawnQueue;
    auto& q = Get();
    if (q.count == 0) return;

    PendingSpawn& next = q.entries[q.head];

    if (next.IsArmed() && Ordeal::IsEquipRequestPending() && q.weaponWait < kWeaponWaitTicks) {
        q.weaponWait++;
        return;
    }

    Actor player(Player::GetRaw());
    if (!Ordeal::Spawn(next.name, player, next.x, next.y, next.z, next.Loadout())) return;

    next.active = false;
    q.head = (q.head + 1) % kMaxPendingSpawns;
    q.count--;
    q.weaponWait = 0;
}
