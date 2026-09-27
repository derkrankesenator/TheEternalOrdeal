#include <wiixlaunch.hpp>
#include <cstring>
#include "wiixlaunch/botw/botw.hpp"
#include <ordeal/pending_spawn.hpp>
#include <ordeal/wave_logic.hpp>

using namespace WiiXLaunch::BotW;

long timeDerk = 0L;
bool wiiinit = false;
float linkX;
float linkY;
float linkZ;
bool p = false;
int currentRupees = 0;
int kc = 0;
int wavenumber = 0;


int alive = 0;
int lastalive = -1;

void CheckForSwordSpawn() {

    if (!Player::ConsumeAttackEvent()) return;

}

/*
 * Vorraum
X: -9.630769, Y: 5.007475, Z: 75.632301 // Links unten von oben nördlich ausgerichtet wo ende norden ist
X: 10.628263, Y: 30.540443, Z: 45.462761 Rechts oben
 * Main
X: -40.629492, Y: 5.005625, Z: 50.631996 // Links unten
X: 40.642975, Y: 30.008885, Z: -50.636909 Rechts oben
*/

bool isInShrine()
{
    if (Player::GetPosition(linkX, linkY, linkZ))
    {
        // Vorraum
        if ((linkX > -10 && linkX < 10 && linkY > 5 && linkY < 30 && linkZ > 45 && linkZ < 76) || (linkX > -40 && linkX < 40 && linkY > 5 && linkY < 30 && linkZ > -50 && linkZ < 50))
        {
            return true;
        }
        else
        {
            return false;
        }
    }
    else
    {
        return false;
    }
}

WIIXL_HOOK_DEFINE_TRAMPOLINE(PlayerTickHook) {
    static void Callback(void* player) {
        timeDerk++;
        ExecutePendingSpawn();
        Orig(player);
        impl::RawPlayerRef() = player;
        if (isInShrine())
        {
            if (alive == 0 && lastalive != alive)
            {
                wavenumber++;
                wave(wavenumber);
            }
            else
            {
                lastalive = alive;
            }
        }
        else
        {
            kc = 0;
        }
    }
};

// from https://github.com/Mindstormman06/BreathOfTheWild_AutoPickup/blob/main/include/autopickup/game.hpp
struct DropTableEntry {
    int16_t count;
    uint8_t posKind;
    uint8_t velKind;
#if WIIXL_SWITCH
    uint32_t _pad;
#endif
    Pouch::impl::SafeString name;
};

struct DropTable {
    int32_t numEntries;
#if WIIXL_SWITCH
    int32_t _pad;
#endif
    DropTableEntry* entries;
};

WIIXL_HOOK_DEFINE_TRAMPOLINE(CreateDropsWithTableHook) {
    // ksys::act::DropMgr::createDropsWithTable(DropMgr* this, Actor* dropper, PendingDrop* pending, DropTable* table)
    static void Callback(void* dropMgr, void* dropper, void* pending, DropTable* table)
    {
        // This runs for EVERY drop table in the game - pots, crates, ore,
        // trees - not just for enemies dying. Only an enemy's death counts,
        // and alive never goes below 0: once it is negative, "alive == 0"
        // can never be true again and no further wave would ever start.
        const char* name = Actor(dropper).GetName();
        if (name && std::strncmp(name, "Enemy_", 6) == 0) {
            kc += 1;
            if (alive > 0) alive -= 1;
        }
        Orig(dropMgr, dropper, pending, table);
    }
};

// Entry point called once at plugin/module load. Install your hooks here.
extern "C" void WiiXLaunch_Init() {
    static bool initialized = false;
    if (initialized) return;
    initialized = true;

#if WIIXL_WIIU
    if (!WiiXLaunch::Backend::InitWiiUBackend()) return;
#elif WIIXL_CEMU
    if (!WiiXLaunch::Backend::InitCemuBackend()) return;
#endif

    WIIXL_LOG("WiiXLaunch: init OK");

    // Detect running game version before installing hooks or patches.
    Player::Init();
    Actor::Init();
    // Every queued spawn counts as one more enemy alive for the wave logic.
    SetOnPendingSpawnAdded([]() { alive += 1; });
    Player::OnTick(CheckForSwordSpawn);
    CreateDropsWithTableHook::Install(0, 0x0310b9c8);
    PlayerTickHook::Install(0x873374, 0x02d67cf4);
    // Register surfaces: core, networking, and base services.
    WiiXLaunch::Core::Register();

#if WIIXL_HAVE_GAME_MODULE
    WIIXL_LOG("WiiXLaunch: game module '%s'", WiiXLaunch::GameModule::kName);
    WiiXLaunch::GameModule::Register();
#else
    WIIXL_LOG("WiiXLaunch: no game module in this build - base surfaces only.");
#endif

    WiiXLaunch::Surface::LogRegistered();

    // Probe filesystem availability at the entry hook.
    WiiXLaunch::LoadPoint::Probe("entry-hook");

    // System clock check.
    char clock[20];
    WiiXLaunch::Time::FormatNow(clock, sizeof(clock));
    WIIXL_LOG("WiiXLaunch: system clock %s", clock);

#if WIIXL_BOTW_DEMO
#if WIIXL_SWITCH
    NVN::Init();
    NVN::RegisterDrawCallback(OnRender);
    // Deferred until the game initializes the NVN device.
    NVN::OnInitialized([]() {
        g_LogoTexture = NVN::CreateTexturePackaged(g_TestPicTextureBytes,
                                                   kTestPicTextureSize);
        WIIXL_LOG("WiiXLaunch: NVN logo texture initialized: %p",
                  reinterpret_cast<void*>(g_LogoTexture));
    });
#elif WIIXL_CEMU
    GX2::Init();
    GX2::RegisterDrawCallback(OnRender);
    GX2::OnInitialized([]() {
        g_LogoTexture = GX2::LoadTexture("WiiXLaunch/mods/_host/logo.bin");
    });
#endif
#endif // WIIXL_BOTW_DEMO
}

