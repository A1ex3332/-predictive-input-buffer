#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <chrono>
#include <deque>
#include <vector>

using namespace geode::prelude;
using Clock = std::chrono::steady_clock;

struct Stage { int all; int wave; const char* name; };
static constexpr Stage STAGES[] = {
    {2, 2, "STAGE 1 (2 All / 2 Wave)"},
    {4, 4, "STAGE 2 (4 All / 4 Wave)"},
    {8, 8, "STAGE 3 (8 All / 8 Wave)"},
    {16, 8, "STAGE 4 (16 All / 8 Wave)"},
    {0, 0, "OFF"},
};
static constexpr int STAGE_COUNT = 5;
static constexpr int BUDGET_MS = 25;      // max time spent judging one click

static int  g_stage = 0;
static bool g_simulating = false;         // inside a secret test or a snapshot restore
static bool g_simDied = false;
static bool g_busy = false;               // re-entry lock

struct Req     { bool down; int button; bool isP1; };
struct Pending { int ticks; bool down; int button; bool isP1; };
static std::deque<Req>     g_incoming;
static std::deque<Pending> g_pending;
static int g_activeDelay = 0;

static bool dbg() { return Mod::get()->getSettingValue<bool>("debug"); }

// Load a snapshot while ignoring any input the game generates because of it
static void restore(PlayLayer* pl, CheckpointObject* cp) {
    bool old = g_simulating;
    g_simulating = true;
    pl->loadFromCheckpoint(cp);
    g_simulating = old;
}

$on_mod(Loaded) {
    log::info("[InputBuffer] ============================================");
    log::info("[InputBuffer] v1.3 LOADED (anti-freeze: restore guard + time limit)");
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

class $modify(PBPlayLayer, PlayLayer) {
    void destroyPlayer(PlayerObject* p, GameObject* g) {
        if (g_simulating) { g_simDied = true; return; }
        PlayLayer::destroyPlayer(p, g);
    }

    void resetLevel() {
        g_incoming.clear(); g_pending.clear(); g_activeDelay = 0;
        PlayLayer::resetLevel();
    }
};

class $modify(PBGameLayer, GJBaseGameLayer) {
    static constexpr float TICK = 1.f / 240.f;

    void step() { GJBaseGameLayer::processCommands(TICK, false, true); }

    bool trial(PlayLayer* pl, int delay, Req r, int look, bool holdMode) {
        g_simulating = true; g_simDied = false;

        for (int i = 0; i < delay && !g_simDied; i++) step();
        if (!g_simDied) {
            GJBaseGameLayer::handleButton(true, r.button, r.isP1);
            if (!holdMode) {
                step();
                GJBaseGameLayer::handleButton(false, r.button, r.isP1);
            }
            for (int i = 0; i < look && !g_simDied; i++) step();
        }

        bool dead = g_simDied
            || (pl->m_player1 && pl->m_player1->m_isDead)
            || (pl->m_player2 && pl->m_player2->m_isDead);
        g_simulating = false;
        return !dead;
    }

    void sendOriginal(Req r) {
        GJBaseGameLayer::handleButton(r.down, r.button, r.isP1);
    }

    void judgeClick(PlayLayer* pl, Req r) {
        auto t0 = Clock::now();
        auto player = r.isP1 ? pl->m_player1 : pl->m_player2;
        bool wave = player && player->m_isDart;
        bool holdMode = player && (player->m_isShip || player->m_isDart);
        int win  = wave ? STAGES[g_stage].wave : STAGES[g_stage].all;
        int look = Mod::get()->getSettingValue<int64_t>(wave ? "wave-lookahead" : "lookahead");

        if (dbg()) log::info("[InputBuffer] judging click (window {}, lookahead {})", win, look);

        Ref<CheckpointObject> now = pl->createCheckpoint();

        bool ok0 = trial(pl, 0, r, look, holdMode);
        restore(pl, now);
        if (ok0) {
            if (dbg()) log::info("[InputBuffer] click safe, unchanged{}", wave ? " (wave)" : "");
            g_activeDelay = 0;
            sendOriginal(r);
            return;
        }

        for (int d = 1; d <= win; d++) {
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
            if (ms > BUDGET_MS) {
                log::info("[InputBuffer] time limit hit at delay {}, input unchanged", d);
                g_activeDelay = 0;
                sendOriginal(r);
                return;
            }

            bool ok = trial(pl, d, r, look, holdMode);
            restore(pl, now);
            if (dbg()) log::info("[InputBuffer]   delay {} -> {}", d, ok ? "SAFE" : "death");
            if (ok) {
                log::info("[InputBuffer] Delayed input by {} ticks{}", d, wave ? " (wave)" : "");
                g_activeDelay = d;
                g_pending.push_back({d, true, r.button, r.isP1});
                return;
            }
        }

        log::info("[InputBuffer] No safe delay found (limit {}), input unchanged", win);
        g_activeDelay = 0;
        sendOriginal(r);
    }

    void judgeRelease(Req r) {
        if (g_activeDelay > 0 || !g_pending.empty())
            g_pending.push_back({g_activeDelay, false, r.button, r.isP1});
        else
            sendOriginal(r);
    }

    // Only catches the input. Judging happens after the tick.
    void handleButton(bool down, int button, bool isP1) {
        auto pl = PlayLayer::get();
        if (g_simulating || g_busy || !pl || g_stage == 4 || !pl->m_isPracticeMode)
            return GJBaseGameLayer::handleButton(down, button, isP1);
        g_incoming.push_back({down, button, isP1});
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        auto pl = PlayLayer::get();
        if (!pl || g_simulating || g_busy || isHalfTick) return;

        g_busy = true;

        // delayed inputs whose time has come
        std::vector<Pending> due;
        for (auto& p : g_pending) p.ticks--;
        for (auto it = g_pending.begin(); it != g_pending.end();) {
            if (it->ticks <= 0) { due.push_back(*it); it = g_pending.erase(it); }
            else ++it;
        }
        for (auto& p : due) GJBaseGameLayer::handleButton(p.down, p.button, p.isP1);

        // new inputs caught during this tick
        std::deque<Req> in;
        in.swap(g_incoming);
        for (auto& r : in) {
            if (r.down) judgeClick(pl, r);
            else        judgeRelease(r);
        }
        g_incoming.clear();   // drop anything generated while judging

        g_busy = false;
    }
};
