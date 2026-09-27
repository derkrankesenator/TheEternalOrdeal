#pragma once

#include <cstdint>

#define WUPS_PLUGIN_NAME_STR "The_Eternal_Ordeal"
#define WUPS_PLUGIN_AUTHOR_STR "derk"
#define WUPS_PLUGIN_VERSION_STR "0.0.1"
#define WUPS_PLUGIN_DESCRIPTION_STR "See"

namespace WiiXLaunch::WiiUConfig {
    constexpr uint64_t TargetTitleIds[] = { 0x00050000101C9400ULL, 0x00050000101C9500ULL, 0x00050000101C9300ULL };
    constexpr uint32_t TargetTitleIdsCount = sizeof(TargetTitleIds) / sizeof(TargetTitleIds[0]);
}
