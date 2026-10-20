#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <deque>

using namespace geode::prelude;

struct Stage { int all; int wave; const char* name; };
static constexpr Stage STAGES[] = {
    {2, 2, "STAGE 1 (2 All / 2 Wave)"},
    {4, 4, "STAGE 2 (4 All / 4 Wave)"},
    {8, 8, "STAGE 3 (8 All / 8 Wave)"},
    {16, 8, "STAGE 4 (16 All / 8 Wave)"},
    {0, 0, "OFF"},
};
static constexpr int STAGE_COUNT = 5;
static int  g_stage = 0;
static bool g_simulating = false;
static bool g_simDied = false;

$on_mod(Loaded) {
    log::info("[InputBuffer] ============================================");
    log::info("[InputBuffer] v1.1 LOADED (check first, delay only if needed)");
    log::info("[InputBuffer] Starting Mode: {}", STAGES[g_stage].name);
    log::info("[InputBuffer] Press F6 in-level to cycle: 2/2 -> 4/4 -> 8/8 -> 16/8 -> OFF");
    log::info("[InputBuffer] ============================================");
}

class $modify(PBKeys, CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool down, bool repeat, double timestamp) {
        if (down && !repeat && key == KEY_F6 && PlayLayer::get()) {
            g_stage = (g_stage + 1) % STAGE_COUNT;
            log::info("[InputBuffer] Mode -> {}", STAGES[g_stage].name);
        }
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat, timestamp);
    }
};

struct PendingInput { int ticks; bool down; int button; bool isP1; };

class $modify(PBPlayLayer, PlayLayer) {
    struct Fields {
        std::deque<PendingInput> pending; // delayed clicks/releases waiting to fire
        int activeDelay = 0;              // delay used by the last click (applied to its release)
    };

    void destroyPlayer(PlayerObject* p, GameObject* g) {
        if (g_simulating) { g_simDied = true; return; }
        PlayLayer::destroyPlayer(p, g);
    }
};

class $modify(PBGameLayer, GJBaseGameLayer) {
    static constexpr float TICK = 1.f / 240.f;

    void step() { GJBaseGameLayer::processCommands(TICK, false, true); }

    // every tick: count down delayed inputs and send them to the real game
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        auto pl = PlayLayer::get();
        if (!pl || g_simulating || isHalfTick) return;

        auto f = static_cast<PBPlayLayer*>(pl)->m_fields.self();
        for (auto& p : f->pending) p.ticks--;
        while (!f->pending.empty() && f->pending.front().ticks <= 0) {
            auto p = f->pending.front();
            f->pending.pop_front();
            GJBaseGameLayer::handleButton(p.down, p.button, p.isP1);
        }
    }

    // secret test: run the game forward, return true if we don't die
    bool survives(int ticks) {
        g_simulating = true; g_simDied = false;
        for (int i = 0; i < ticks && !g_simDied; i++) step();
        g_simulating = false;
        return !g_simDied;
    }

    // ALL player input comes through here first
    void handleButton(bool down, int button, bool isP1) {
        auto pl = PlayLayer::get();
        if (!pl || g_simulating || g_stage == 4 || !pl->m_isPracticeMode)
            return GJBaseGameLayer::handleButton(down, button, isP1);

        auto f = static_cast<PBPlayLayer*>(pl)->m_fields.self();

        // release: give it the same delay as its click so the hold length stays the same
        if (!down) {
            if (f->activeDelay > 0 || !f->pending.empty()) {
                f->pending.push_back({f->activeDelay, false, button, isP1});
                return;
            }
            return GJBaseGameLayer::handleButton(down, button, isP1);
        }

        // click: pick the window for this gamemode
        auto player = isP1 ? pl->m_player1 : pl->m_player2;
        bool wave = player && player->m_isDart;
        int win  = wave ? STAGES[g_stage].wave : STAGES[g_stage].all;
        int look = Mod::get()->getSettingValue<int64_t>(wave ? "wave-lookahead" : "lookahead");

        auto now = pl->createCheckpoint(); // state before the test

        // delay 0 = the player's real click, then 1, 2, 3 ... up to the limit
        for (int delay = 0; delay <= win; delay++) {
            for (int i = 0; i < delay; i++) step();                // wait `delay` ticks
            GJBaseGameLayer::handleButton(true, button, isP1);     // then click
            bool ok = survives(look);
            pl->loadFromCheckpoint(now);                           // undo the test

            if (ok) {
                f->activeDelay = delay;
                if (delay == 0) {
                    // safe as it is: send it unchanged
                    return GJBaseGameLayer::handleButton(true, button, isP1);
                }
                log::info("[InputBuffer] Delayed input by {} ticks{}", delay, wave ? " (wave)" : "");
                f->pending.push_back({delay, true, button, isP1});
                return;
            }
        }

        // no delay in the window survives: send the original click
        f->activeDelay = 0;
        log::info("[InputBuffer] No safe delay found, input unchanged");
        GJBaseGameLayer::handleButton(true, button, isP1);
    }
};
