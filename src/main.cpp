#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <vector>

using namespace geode::prelude;

struct Stage { const char* name; int allLimit; int waveLimit; };
static const Stage STAGES[] = {
    {"STAGE 1: 4 Ticks All",                 4,  -1},
    {"STAGE 2: 8 Ticks All",                 8,  -1},
    {"STAGE 3: 8 Ticks All | 8 Ticks Wave",  8,   8},
    {"STAGE 4: 16 Ticks All | 8 Ticks Wave", 16,  8},
    {"STAGE 5: OFF (100% Raw Input)",        0,   0},
};

static int  g_stage = 1;
static bool g_waveMode = false;
static int  g_tick = 0;

struct Pending { int tick0; int button; bool player1; };
static std::vector<Pending> g_pending;
static bool g_refiring = false;

static int curLimit() {
    auto& s = STAGES[g_stage];
    return (g_waveMode && s.waveLimit >= 0) ? s.waveLimit : s.allLimit;
}

class $modify(BufferLayer, GJBaseGameLayer) {

    int tick() {
        return g_tick;
    }
    void handleButton(bool down, int button, bool isPlayer1) {
        if (g_refiring || !Mod::get()->getSettingValue<bool>("enabled") || curLimit() == 0) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }
        if (down) {
            g_pending.push_back({tick(), button, isPlayer1});
        } else {
            std::erase_if(g_pending, [&](auto& p) {
                return p.button == button && p.player1 == isPlayer1;
            });
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    void processCommands(float dt) {
        GJBaseGameLayer::processCommands(dt);
                g_tick++;

        int now = tick();
        int limit = curLimit();

        for (auto it = g_pending.begin(); it != g_pending.end();) {
            int age = now - it->tick0;
            if (age > limit) { it = g_pending.erase(it); continue; }

            auto* player = it->player1 ? m_player1 : m_player2;
            if (age > 0 && player && player->m_isOnGround) {
                g_refiring = true;
                GJBaseGameLayer::handleButton(false, it->button, it->player1);
                GJBaseGameLayer::handleButton(true,  it->button, it->player1);
                g_refiring = false;

                log::info("[InputBuffer] *** SAVED +{} TICKS ({:.2f} frames @ 60fps) PRESS! "
                          "(Frame0: {} -> Delayed: {}) [Limit: {}] ***",
                          age, age * 0.25f, it->tick0, now, limit);
                it = g_pending.erase(it);
                continue;
            }
            ++it;
        }
    }
};

class $modify(KeyHook, CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool down, bool repeat) {
        if (down && !repeat) {
            if (key == KEY_F6) {
                g_stage = (g_stage + 1) % 5;
                g_pending.clear();
                log::info("[InputBuffer] F6 Cycle -> {}", STAGES[g_stage].name);
            } else if (key == KEY_F7) {
                g_waveMode = !g_waveMode;
                log::info("[InputBuffer] Wave mode {} [Limit: {}]", g_waveMode ? "ON" : "OFF", curLimit());
            }
        }
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat);
    }
};
