#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>

using namespace geode::prelude;

// Late-press forgiveness: a jump pressed up to N ticks AFTER you left the ground
// still counts, as if you had pressed on time.
struct Stage { const char* name; int limit; };
static const Stage STAGES[] = {
    {"STAGE 1: 2 Ticks", 2},
    {"STAGE 2: 4 Ticks", 4},
    {"STAGE 3: 8 Ticks", 8},
    {"STAGE 4: 16 Ticks", 16},
    {"STAGE 5: OFF (100% Raw Input)", 0},
};

static int g_stage = 0;
static int g_tick = 0;
static int g_lastGround[2] = {-100000, -100000};
static int g_force[2] = {0, 0};

static int curLimit() { return STAGES[g_stage].limit; }

class $modify(BufferLayer, GJBaseGameLayer) {

    void handleButton(bool down, int button, bool isPlayer1) {
        if (down && button == 1 && curLimit() > 0 &&
            Mod::get()->getSettingValue<bool>("enabled")) {
            int idx = isPlayer1 ? 0 : 1;
            auto* p = isPlayer1 ? m_player1 : m_player2;
            int late = g_tick - g_lastGround[idx];
            if (p && !p->m_isOnGround && late > 0 && late <= curLimit()) {
                g_force[idx] = 1;
                log::info("[InputBuffer] *** SAVED +{} TICKS ({:.2f} frames @ 60fps) PRESS! "
                          "(Frame0: {} -> Delayed: {}) [Limit: {}] ***",
                          late, late * 0.25f, g_lastGround[idx], g_tick, curLimit());
            }
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        // make the game treat the player as grounded for this step so the jump registers
        if (g_force[0] > 0 && m_player1) { m_player1->m_isOnGround = true; g_force[0]--; }
        if (g_force[1] > 0 && m_player2) { m_player2->m_isOnGround = true; g_force[1]--; }

        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        g_tick++;

        if (m_player1 && m_player1->m_isOnGround) g_lastGround[0] = g_tick;
        if (m_player2 && m_player2->m_isOnGround) g_lastGround[1] = g_tick;
    }
};

class $modify(KeyHook, CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool down, bool repeat, double timestamp) {
        if (down && !repeat && key == KEY_F6) {
            g_stage = (g_stage + 1) % 5;
            log::info("[InputBuffer] F6 Cycle -> {}", STAGES[g_stage].name);
        }
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat, timestamp);
    }
};
