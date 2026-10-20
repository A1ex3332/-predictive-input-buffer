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
static constexpr int BUDGET_MS = 25;

static int  g_stage = 0;
static bool g_simulating = false;
static bool g_simDied = false;
static bool g_passthrough = false;
static PlayerObject* g_dummy = nullptr;
static GameObject*   g_killer = nullptr;
static int g_lastTicks = 0;

struct Pending { int ticks; bool down; int button; bool isP1; };
static std::deque<Pending> g_pending;
static int g_activeDelay = 0;

static bool dbg() { return Mod::get()->getSettingValue<bool>("debug"); }
static bool isZero(CCPoint p) { return p.x == 0.f && p.y == 0.f; }

static CCPoint realPos(PlayerObject* p) {
    CCPoint a = p->getPosition();
    if (!isZero(a)) return a;
    return p->m_position;
}
static CCPoint realLastPos(PlayerObject* p) {
    CCPoint a = p->m_lastPosition;
    if (!isZero(a)) return a;
    return realPos(p);
}

$on_mod(Loaded) {
    log::info("[InputBuffer] ============================================");
    log::info("[InputBuffer] v2.5 LOADED (dummy trajectory + movement fix)");
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
        if (p == g_dummy || g_simulating) {
            g_simDied = true;
            g_killer = g;
            return;
        }
        PlayLayer::destroyPlayer(p, g);
    }

    void resetLevel() {
        g_pending.clear(); g_activeDelay = 0;
        PlayLayer::resetLevel();
    }

    void onQuit() {
        if (g_dummy) { g_dummy->release(); g_dummy = nullptr; }
        g_pending.clear();
        PlayLayer::onQuit();
    }
};

class $modify(PBGameLayer, GJBaseGameLayer) {
    static constexpr float TICK = 1.f / 240.f;

    PlayerObject* getDummy() {
        if (!g_dummy) {
            g_dummy = PlayerObject::create(1, 1, this, this, true);
            g_dummy->retain();
            g_dummy->setVisible(false);
        }
        return g_dummy;
    }

    void copyState(PlayerObject* d, PlayerObject* real) {
        d->copyAttributes(real);

        d->m_isShip   = real->m_isShip;
        d->m_isBird   = real->m_isBird;
        d->m_isBall   = real->m_isBall;
        d->m_isDart   = real->m_isDart;
        d->m_isRobot  = real->m_isRobot;
        d->m_isSpider = real->m_isSpider;
        d->m_isSwing  = real->m_isSwing;

        CCPoint pos  = realPos(real);
        CCPoint last = realLastPos(real);
        d->setPosition(pos);
        d->m_position     = pos;
        d->m_lastPosition = last;
        d->m_vehicleSize  = real->m_vehicleSize;
        d->m_yVelocity    = real->m_yVelocity;
        d->m_isOnGround   = real->m_isOnGround;
        d->m_isUpsideDown = real->m_isUpsideDown;
        d->m_playerSpeed  = real->m_playerSpeed;
        d->setRotation(real->getRotation());
        d->m_isDead = false;
    }

    bool trial(PlayerObject* real, int delay, bool holdMode, int look) {
        auto d = getDummy();
        copyState(d, real);

        CCPoint start = d->getPosition();
        g_simulating = true; g_simDied = false; g_killer = nullptr;

        int total = delay + look;
        int t = 0;
        for (; t < total && !g_simDied && !d->m_isDead; t++) {
            if (t == delay) d->pushButton(PlayerButton::Jump);
            if (!holdMode && t == delay + 1) d->releaseButton(PlayerButton::Jump);

            // remember where we were, step, then sync both position fields by hand
            CCPoint before = d->getPosition();
            d->m_lastPosition = before;
            d->update(TICK);
            CCPoint after = d->getPosition();

            bool nodeUnchanged = (after.x == before.x && after.y == before.y);
            bool fieldMoved = (d->m_position.x != before.x || d->m_position.y != before.y);
            if (nodeUnchanged && fieldMoved) d->setPosition(d->m_position);  // physics moved the field only
            else d->m_position = after;                                      // node moved: copy to field

            this->checkCollisions(d, TICK, false);
            d->updateRotation(TICK);

            if (dbg() && delay == 0 && t < 4) {
                auto p = d->getPosition();
                log::info("[InputBuffer]     tick {}: ({:.2f},{:.2f}) field=({:.2f},{:.2f})",
                    t, p.x, p.y, d->m_position.x, d->m_position.y);
            }
        }
        d->releaseButton(PlayerButton::Jump);

        bool dead = g_simDied || d->m_isDead;
        g_simulating = false;
        g_lastTicks = t;

        if (dbg() && delay == 0) {
            auto e = d->getPosition();
            log::info("[InputBuffer]   dummy: ran {} ticks, ({:.1f},{:.1f}) -> ({:.1f},{:.1f}), {}, killer object id {}",
                t, start.x, start.y, e.x, e.y, dead ? "DEAD" : "alive",
                g_killer ? g_killer->m_objectID : -1);
        }
        return !dead;
    }

    void judgeClick(PlayLayer* pl, bool down, int button, bool isP1) {
        auto t0 = Clock::now();
        PlayerObject* p1 = pl->m_player1;
        PlayerObject* p2 = pl->m_player2;

        // pick the player the flag points to; if it sits at (0,0), use the other one
        PlayerObject* player = isP1 ? p1 : p2;
        if (!player || isZero(realPos(player))) {
            PlayerObject* alt = isP1 ? p2 : p1;
            if (alt && !isZero(realPos(alt))) player = alt;
        }
        if (!player) return GJBaseGameLayer::handleButton(down, button, isP1);

        bool wave = player->m_isDart;
        bool holdMode = player->m_isShip || player->m_isDart;
        int win  = wave ? STAGES[g_stage].wave : STAGES[g_stage].all;
        int look = Mod::get()->getSettingValue<int64_t>(wave ? "wave-lookahead" : "lookahead");

        if (trial(player, 0, holdMode, look)) {
            if (dbg()) log::info("[InputBuffer] click safe, unchanged{}", wave ? " (wave)" : "");
            g_activeDelay = 0;
            return GJBaseGameLayer::handleButton(down, button, isP1);
        }

        if (g_lastTicks <= 2) {
            log::info("[InputBuffer] SIM BROKEN: dummy died after {} ticks, input unchanged", g_lastTicks);
            g_activeDelay = 0;
            return GJBaseGameLayer::handleButton(down, button, isP1);
        }

        for (int dly = 1; dly <= win; dly++) {
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
            if (ms > BUDGET_MS) {
                log::info("[InputBuffer] time limit hit at delay {}, input unchanged", dly);
                g_activeDelay = 0;
                return GJBaseGameLayer::handleButton(down, button, isP1);
            }
            bool ok = trial(player, dly, holdMode, look);
            if (dbg()) log::info("[InputBuffer]   delay {} -> {} (ran {} ticks)", dly, ok ? "SAFE" : "death", g_lastTicks);
            if (ok) {
                log::info("[InputBuffer] Delayed input by {} ticks{}", dly, wave ? " (wave)" : "");
                g_activeDelay = dly;
                g_pending.push_back({dly, true, button, isP1});
                return;
            }
        }

        log::info("[InputBuffer] No safe delay found (limit {}), input unchanged", win);
        g_activeDelay = 0;
        GJBaseGameLayer::handleButton(down, button, isP1);
    }

    void handleButton(bool down, int button, bool isP1) {
        auto pl = PlayLayer::get();
        if (g_passthrough || g_simulating || !pl || g_stage == 4 || !pl->m_isPracticeMode)
            return GJBaseGameLayer::handleButton(down, button, isP1);

        if (!down) {
            if (g_activeDelay > 0 || !g_pending.empty()) {
                g_pending.push_back({g_activeDelay, false, button, isP1});
                return;
            }
            return GJBaseGameLayer::handleButton(down, button, isP1);
        }

        judgeClick(pl, down, button, isP1);
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        if (g_simulating || isHalfTick || g_pending.empty()) return;

        std::vector<Pending> due;
        for (auto& p : g_pending) p.ticks--;
        for (auto it = g_pending.begin(); it != g_pending.end();) {
            if (it->ticks <= 0) { due.push_back(*it); it = g_pending.erase(it); }
            else ++it;
        }
        g_passthrough = true;
        for (auto& p : due) GJBaseGameLayer::handleButton(p.down, p.button, p.isP1);
        g_passthrough = false;
    }
};
