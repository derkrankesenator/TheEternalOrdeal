#pragma once
#include "pending_spawn.hpp"

#include <cstring>
inline int CountLivingEnemies() {
    int count = 0;
    WiiXLaunch::BotW::Actor::ForEachDynamic([&](const WiiXLaunch::BotW::Actor& actor) {
        const char* name = actor.GetName();
        if (!name || std::strncmp(name, "Enemy_", 6) != 0) return true;
        const uint8_t state = actor.GetState();
        if (state == 3) return true;                                 // being deleted
        if (state != 0 && actor.GetCurrentLife() <= 0) return true;  // dead
        ++count;
        return true;
    });
    return count;
}

constexpr int kQuietTicksBeforeWave = 60;

constexpr int kEnemyCountInterval = 15;

struct WaveState {
    int quietTicks = 0;
    int lastAlive = 0;
    int countdown = 0;
};

inline WaveState& GetWaveState() {
    static WaveState s;
    return s;
}

inline void wave(int wavenumber) {

    const char* enmy = "Enemy_Bokoblin_Junior";
    if (wavenumber == 1)
    {
        enmy = "Enemy_Bokoblin_Junior";
    }
    else if (wavenumber == 2)
    {
        enmy = "Enemy_Bokoblin_Middle";
    }
    else if (wavenumber == 3)
    {
        enmy = "Enemy_Bokoblin_Senior";
    }
    else if (wavenumber == 4)
    {
        enmy = "Enemy_Bokoblin_Dark";
    }
    else
    {
        enmy = "Enemy_Bokoblin_Gold";
    }


    AddPendingSpawn(enmy,0.228444f, 9.999949f, -7.635260f, {.weapon = "Weapon_Sword_033", .shield = "Weapon_Shield_033"}); // "Obj_BoxMystery"

    // Lookout
    AddPendingSpawn(enmy, 12.074479f, 15.499956f, -21.948946f, {.bow = "Weapon_Bow_033", .arrow = "FireArrow"});
    AddPendingSpawn(enmy, 11.980627f, 15.500957f, 6.304846f, {.bow = "Weapon_Bow_033", .arrow = "FireArrow"});
    AddPendingSpawn(enmy, -12.239229f, 15.499956f, 5.858237f, {.bow = "Weapon_Bow_033", .arrow = "FireArrow"});
    AddPendingSpawn(enmy, -11.973566, 15.499956, -21.647131, {.bow = "Weapon_Bow_033", .arrow = "FireArrow"});

    // Outside
    AddPendingSpawn(enmy, -19.783644, 10.007988, -7.984582, {.weapon = "Weapon_Sword_033", .shield = "Weapon_Shield_033"});
    AddPendingSpawn(enmy, 0.118927, 10.048508, 11.794467, {.weapon = "Weapon_Sword_033", .shield = "Weapon_Shield_033"});
    AddPendingSpawn(enmy, 19.626371, 10.005018, -7.780757, {.weapon = "Weapon_Sword_033", .shield = "Weapon_Shield_033"});
    AddPendingSpawn(enmy, -0.017828, 10.053064, -27.817570, {.weapon = "Weapon_Sword_033", .shield = "Weapon_Shield_033"});
    // Inside
    AddPendingSpawn(enmy, 7.051447, 9.999949, -14.903265, {.weapon = "Weapon_Sword_033", .shield = "Weapon_Shield_033"});
    AddPendingSpawn(enmy, -6.825766, 9.999949, -14.900736, {.weapon = "Weapon_Sword_033", .shield = "Weapon_Shield_033"});
    AddPendingSpawn(enmy, -6.961237, 9.999949, -1.219597, {.weapon = "Weapon_Sword_033", .shield = "Weapon_Shield_033"});
    AddPendingSpawn(enmy, 6.809532, 9.999949, -1.228581, {.weapon = "Weapon_Sword_033", .shield = "Weapon_Shield_033"});
}


inline void ResetWaves(int& wavenumber) {
    wavenumber = 0;
    ClearPendingSpawns();
    GetWaveState() = WaveState{};
}


inline bool ResetWavesOnReload(void* player, int& wavenumber) {
    static uint32_t lastPlayerId = 0;
    const uint32_t id = WiiXLaunch::BotW::Actor(player).GetId();
    if (id == 0 || id == lastPlayerId) return false;   // 0 = unreadable, try again next tick

    const bool reloaded = lastPlayerId != 0;   // the very first player is not a reload
    lastPlayerId = id;
    if (reloaded) ResetWaves(wavenumber);
    return reloaded;
}

inline int UpdateWaves(int& wavenumber) {
    WaveState& w = GetWaveState();
    if (--w.countdown <= 0) {
        w.countdown = kEnemyCountInterval;
        w.lastAlive = CountLivingEnemies();
    }

    if (PendingSpawnCount() > 0 || w.lastAlive > 0) {
        w.quietTicks = 0;
        return w.lastAlive;
    }

    if (++w.quietTicks < kQuietTicksBeforeWave) return w.lastAlive;

    w.quietTicks = 0;
    ++wavenumber;
    wave(wavenumber);
    return w.lastAlive;
}

/*
 Middle
0.228444f, 9.999949f, -7.635260
 Lookout
12.074479f, 15.499956f, -21.948946
11.980627f, 15.500957f, 6.304846
-12.239229f, 15.499956f, 5.858237
-11.973566, 15.499956, -21.647131
 Outside
-19.783644, 10.007988, -7.984582);
0.118927, 10.048508, 11.794467);
19.626371, 10.005018, -7.780757);
-0.017828, 10.053064, -27.817570);
 Inside
7.051447, 9.999949, -14.903265);
-6.825766, 9.999949, -14.900736);
-6.961237, 9.999949, -1.219597);
6.809532, 9.999949, -1.228581);



*/


#ifndef WIIXLAUNCH_WAVE_LOGIC_H
#define WIIXLAUNCH_WAVE_LOGIC_H

#endif //WIIXLAUNCH_WAVE_LOGIC_H
