#pragma once
#include "pending_spawn.hpp"

// inline: this header defines a function, so without it a second .cpp
// including the header would fail to link with a duplicate definition.
inline void wave(int wavenumber) {
 AddPendingSpawn("Enemy_Bokoblin_Senior", 0.228444f, 9.999949f, -7.635260f, {.weapon = "Weapon_Sword_033", .shield = "Weapon_Shield_033"});
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
