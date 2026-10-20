#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <cstdio>

#ifdef GEODE_IS_WINDOWS
#define NOMINMAX
#include <windows.h>
#endif

using namespace geode::prelude;

// ---------- stages: {all limit, wave limit} ----------
struct Stage { int all; int wave; };
static const Stage STAGES[] = { {2,2}, {4,4}, {8,8}, {16,8}, {0,0} };
static const int NUM_STAGES = 5;

static int g_stage = 0;
static int g_tick = 0;
static int g_lastGround[2] = {-100000, -100000};
static int g_force[2] = {0, 0};
static int g_saved = 0;

static int curLimit() { return STAGES[g_stage].all; }

static std::string stageName() {
    auto& s = STAGES[g_stage];
    if (s.all == 0) return "OFF (100% Raw Input)";
    return fmt::format("STAGE {} ({} All / {} Wave)", g_stage + 1, s.all, s.wave);
}

// ---------- own live console ----------
static void clog(std::string const& msg) {
    std::string line = "[InputBuffer] " + msg + "\n";
    fputs(line.c_str(), stdout);
    fflush(stdout);
}

$on_mod(Loaded) {
#ifdef GEODE_IS_WINDOWS
    if (AllocConsole()) {
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        SetConsoleTitleA("Geode Predictive Input Buffer - Live Console");
    }
#endif
    clog("==========================================================");
    clog("v0.2 LOADED (ground late-press forgiveness)");
    clog("Starting Mode: " + stageName());
    clog("Press F6 in-level to cycle: 2/2 -> 4/4 -> 8/8 -> 16/8 -> OFF");
    clog("Note: wave limit is shown but only ground jumps are handled for now");
    clog("==========================================================");
}

// ---------- gameplay hook ----------
class $modify(BufferLayer, GJBaseGameLayer) {

    void handleButton(bool down, int button, bool isPlayer1) {
        if (down && button == 1 && curLimit() > 0 &&
            Mod::get()->getSettingValue<bool>("enabled")) {
            int idx = isPlayer1 ? 0 : 1;
            auto* p = isPlayer1 ? m_player1 : m_player2;
            int late = g_tick - g_lastGround[idx];
            if (p && !p->m_isOnGround && late > 0 && late <= curLimit()) {
                g_force[idx] = 1;
                g_saved++;
                clog(fmt::format(
                    "*** SAVED +{} TICKS ({:.2f} frames @ 60fps) PRESS! "
                    "(Frame0: {} -> Delayed: {}) [Limit: {}] *** total: {}",
                    late, late * 0.25f, g_lastGround[idx], g_tick, curLimit(), g_saved));
            }
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        // treat the player as grounded for this step so the late jump registers
        if (g_force[0] > 0 && m_player1) { m_player1->m_isOnGround = true; g_force[0]--; }
        if (g_force[1] > 0 && m_player2) { m_player2->m_isOnGround = true; g_force[1]--; }

        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        g_tick++;

        if (m_player1 && m_player1->m_isOnGround) g_lastGround[0] = g_tick;
        if (m_player2 && m_player2->m_isOnGround) g_lastGround[1] = g_tick;
    }
};

// ---------- F6 stage cycle ----------
class $modify(KeyHook, CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool down, bool repeat, double timestamp) {
        if (down && !repeat && key == KEY_F6) {
            g_stage = (g_stage + 1) % NUM_STAGES;
            clog("F6 Cycle -> " + stageName());
        }
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat, timestamp);
    }
};
