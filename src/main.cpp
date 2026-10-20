#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
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
static constexpr int BUDGET_MS = 60;

static int  g_stage = 0;
static bool g_simulating = false;
static bool g_simDied = false;
static bool g_passthrough = false;
static PlayerObject* g_dummy = nullptr;
static GameObject*   g_killer = nullptr;
static int g_lastTicks = 0;
static bool g_heldReal = false;

// path of the most recent trial (one point per tick)
static std::vector<CCPoint> g_trialPath;

// prediction check: compare the chosen trial's path with what the real icon does
static bool g_verify = false;
static std::vector<CCPoint> g_vPath;
static int  g_vTick = 0;
static PlayerObject* g_vPlayer = nullptr;
static float g_vErr[3] = {0, 0, 0};   // summed error for alignment offsets -1, 0, +1
static float g_vMax = 0.f;
static bool  g_vDriftLogged = false;

// last prediction for the player's own click (to compare with the real death)
static bool    g_havePred = false;
static bool    g_predDead = false;
static int     g_predTicks = 0;
static CCPoint g_predEnd = {0, 0};
static int     g_predKiller = -1;

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
static bool nearOrigin(PlayerObject* p) { return realPos(p).x < 50.f; }
static float dist(CCPoint a, CCPoint b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y)); }

static void startVerify(PlayerObject* player) {
    g_vPath = g_trialPath;
    g_vTick = 0;
    g_vPlayer = player;
    g_vErr[0] = g_vErr[1] = g_vErr[2] = 0.f;
    g_vMax = 0.f;
    g_vDriftLogged = false;
    g_verify = !g_vPath.empty();
}

static void stopVerify() { g_verify = false; g_vPlayer = nullptr; }

static void verifyStep() {
    if (!g_vPlayer) { g_verify = false; return; }
    CCPoint r = realPos(g_vPlayer);
    int i = g_vTick++;

    if (i < (int)g_vPath.size()) {
        float e = dist(r, g_vPath[i]);
        g_vMax = std::max(g_vMax, e);
        if (e > 3.f && !g_vDriftLogged) {
            g_vDriftLogged = true;
            log::info("[InputBuffer]   CHECK: prediction drifted at tick {}: real ({:.1f},{:.1f}) vs predicted ({:.1f},{:.1f}), error {:.1f}",
                i, r.x, r.y, g_vPath[i].x, g_vPath[i].y, e);
        }
    }
    for (int o = -1; o <= 1; o++) {
        int idx = i + o;
        if (idx >= 0 && idx < (int)g_vPath.size()) g_vErr[o + 1] += dist(r, g_vPath[idx]);
    }

    if (g_vTick >= 60 || g_vTick >= (int)g_vPath.size()) {
        int best = 0;
        for (int k = 1; k < 3; k++) if (g_vErr[k] < g_vErr[best]) best = k;
        int off = best - 1;
        log::info("[InputBuffer]   CHECK result: max error {:.1f} units over {} ticks, best alignment offset {} {}",
            g_vMax, g_vTick, off,
            off == 0 ? "(timing matches)" :
            off > 0  ? "(real game is AHEAD of the prediction: try delay-offset +1)"
                     : "(real game is BEHIND the prediction: try delay-offset -1)");
        stopVerify();
    }
}

$on_mod(Loaded) {
    log::info("[InputBuffer] ============================================");
    log::info("[InputBuffer] v3.1 LOADED (settle lookahead, margin, offset, prediction check)");
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

        if (dbg() && p) {
            auto rp = realPos(p);
            log::info("[InputBuffer] REAL death at ({:.1f},{:.1f}), object id {}",
                rp.x, rp.y, g ? g->m_objectID : -1);
            if (g_havePred) {
                log::info("[InputBuffer]   last prediction for the click: {} after {} ticks at ({:.1f},{:.1f}), object id {}",
                    g_predDead ? "DEAD" : "alive", g_predTicks, g_predEnd.x, g_predEnd.y, g_predKiller);
            }
        }
        stopVerify();
        PlayLayer::destroyPlayer(p, g);
    }

    void resetLevel() {
        g_pending.clear(); g_activeDelay = 0; g_heldReal = false;
        stopVerify();
        PlayLayer::resetLevel();
    }

    void onQuit() {
        if (g_dummy) { g_dummy->release(); g_dummy = nullptr; }
        g_pending.clear(); g_heldReal = false;
        stopVerify();
        PlayLayer::onQuit();
    }
};

class $modify(PBGameLayer, GJBaseGameLayer) {
    static constexpr float TICK = 1.f / 240.f;
    static constexpr float DT   = TICK * 60.f;

    void sendReal(bool down, int button, bool isP1) {
        if (button == 1) g_heldReal = down;
        GJBaseGameLayer::handleButton(down, button, isP1);
    }

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

    // One test run of the dummy.
    //   startHeld : button already held when the test starts (release tests)
    //   pressAt / releaseAt : tick of the press / release (-1 = never)
    //   settle    : after `look` ticks keep running until the dummy lands (max +240 ticks)
    bool trial(PlayerObject* real, int look, bool startHeld, int pressAt, int releaseAt,
               bool original, bool settle) {
        auto d = getDummy();
        d->releaseButton(PlayerButton::Jump);
        copyState(d, real);
        if (startHeld) d->pushButton(PlayerButton::Jump);

        CCPoint start = d->getPosition();
        g_simulating = true; g_simDied = false; g_killer = nullptr;
        g_trialPath.clear();

        int last = std::max(std::max(pressAt, releaseAt), 0);
        int total = last + look;
        int hardCap = last + 240;
        int t = 0;
        for (;; t++) {
            if (g_simDied || d->m_isDead) break;
            if (t >= total && !(settle && !d->m_isOnGround && t < hardCap)) break;

            if (t == pressAt)   d->pushButton(PlayerButton::Jump);
            if (t == releaseAt) d->releaseButton(PlayerButton::Jump);

            CCPoint before = d->getPosition();
            d->m_lastPosition = before;
            d->update(DT);
            CCPoint after = d->getPosition();

            bool nodeUnchanged = (after.x == before.x && after.y == before.y);
            bool fieldMoved = (d->m_position.x != before.x || d->m_position.y != before.y);
            if (nodeUnchanged && fieldMoved) d->setPosition(d->m_position);
            else d->m_position = after;

            this->checkCollisions(d, DT, false);
            d->updateRotation(DT);

            g_trialPath.push_back(d->getPosition());
        }
        d->releaseButton(PlayerButton::Jump);

        bool dead = g_simDied || d->m_isDead;
        g_simulating = false;
        g_lastTicks = t;

        if (original) {
            auto e = d->getPosition();
            g_havePred = true; g_predDead = dead; g_predTicks = t; g_predEnd = e;
            g_predKiller = g_killer ? g_killer->m_objectID : -1;
            if (dbg()) {
                log::info("[InputBuffer]   dummy: ran {} ticks, ({:.1f},{:.1f}) -> ({:.1f},{:.1f}), {}, killer object id {}",
                    t, start.x, start.y, e.x, e.y, dead ? "DEAD" : "alive", g_predKiller);
            }
        }
        return !dead;
    }

    PlayerObject* pickPlayer(PlayLayer* pl, bool isP1) {
        PlayerObject* p1 = pl->m_player1;
        PlayerObject* p2 = pl->m_player2;
        PlayerObject* player = isP1 ? p1 : p2;
        PlayerObject* alt    = isP1 ? p2 : p1;
        if (!player || (nearOrigin(player) && alt && !nearOrigin(alt))) {
            if (alt) player = alt;
        }
        return player;
    }

    bool clickPending() {
        for (auto& p : g_pending) if (p.down) return true;
        return false;
    }

    // ---------------- CLICK ----------------
    void judgeClick(PlayLayer* pl, bool down, int button, bool isP1) {
        auto t0 = Clock::now();
        PlayerObject* player = pickPlayer(pl, isP1);
        if (!player) return sendReal(down, button, isP1);

        bool wave = player->m_isDart;
        bool holdMode = player->m_isShip || player->m_isDart;
        bool settle = !(holdMode || player->m_isBird || player->m_isSwing);
        int win  = wave ? STAGES[g_stage].wave : STAGES[g_stage].all;
        int look = Mod::get()->getSettingValue<int64_t>(wave ? "wave-lookahead" : "lookahead");
        int margin = Mod::get()->getSettingValue<int64_t>("safety-margin");
        int offset = Mod::get()->getSettingValue<int64_t>("delay-offset");

        auto runClick = [&](int delay, bool original) {
            return trial(player, look, false, delay, holdMode ? -1 : delay + 1, original, settle);
        };
        // a delay is safe only if it AND the next `margin` ticks are safe
        auto okAt = [&](int delay, bool original) {
            for (int m = 0; m <= margin; m++)
                if (!runClick(delay + m, original && m == 0)) return false;
            return true;
        };

        bool ok0 = runClick(0, true);
        int ticks0 = g_lastTicks;
        bool ok0m = ok0;
        for (int m = 1; m <= margin && ok0m; m++) ok0m = runClick(m, false);

        if (ok0m) {
            if (margin > 0) runClick(0, false);   // refresh the path for the check
            if (dbg()) log::info("[InputBuffer] click safe, unchanged{}", wave ? " (wave)" : "");
            startVerify(player);
            g_activeDelay = 0;
            return sendReal(down, button, isP1);
        }

        if (!ok0 && ticks0 <= 2) {
            log::info("[InputBuffer] SIM BROKEN: dummy died after {} ticks, input unchanged", ticks0);
            g_activeDelay = 0;
            return sendReal(down, button, isP1);
        }

        for (int dly = 1; dly <= win; dly++) {
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
            if (ms > BUDGET_MS) {
                log::info("[InputBuffer] time limit hit at delay {}, input unchanged", dly);
                g_activeDelay = 0;
                return sendReal(down, button, isP1);
            }
            bool ok = okAt(dly, false);
            if (dbg()) log::info("[InputBuffer]   click delay {} -> {}", dly, ok ? "SAFE" : "death");
            if (ok) {
                if (margin > 0) runClick(dly, false);   // refresh the path for the check
                int real = std::max(0, dly + offset);
                log::info("[InputBuffer] Delayed CLICK by {} ticks{}{}", dly,
                    offset != 0 ? " (offset applied)" : "", wave ? " (wave)" : "");
                startVerify(player);
                g_activeDelay = real;
                g_pending.push_back({real, true, button, isP1});
                return;
            }
        }

        log::info("[InputBuffer] No safe click delay found (limit {}), input unchanged", win);
        g_activeDelay = 0;
        sendReal(down, button, isP1);
    }

    // ---------------- RELEASE ----------------
    void judgeRelease(PlayLayer* pl, int button, bool isP1) {
        auto t0 = Clock::now();
        PlayerObject* player = pickPlayer(pl, isP1);

        int base = g_activeDelay;
        int chosen = base;

        bool fix = Mod::get()->getSettingValue<bool>("fix-releases");
        bool hold = player && (player->m_isShip || player->m_isDart);

        if (fix && hold && !clickPending() && g_heldReal) {
            bool wave = player->m_isDart;
            int win  = wave ? STAGES[g_stage].wave : STAGES[g_stage].all;
            int look = Mod::get()->getSettingValue<int64_t>(wave ? "wave-lookahead" : "lookahead");

            bool found = false;
            for (int extra = 0; extra <= win; extra++) {
                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
                if (ms > BUDGET_MS) {
                    log::info("[InputBuffer] time limit hit while checking release, using base timing");
                    break;
                }
                bool ok = trial(player, look, true, -1, base + extra, false, false);
                if (dbg() && extra <= 1) log::info("[InputBuffer]   release +{} -> {}", extra, ok ? "SAFE" : "death");
                if (ok) {
                    chosen = base + extra;
                    found = true;
                    if (extra > 0) log::info("[InputBuffer] Delayed RELEASE by {} extra ticks{}", extra, wave ? " (wave)" : "");
                    else if (dbg()) log::info("[InputBuffer] release safe{}", wave ? " (wave)" : "");
                    break;
                }
            }
            if (!found) log::info("[InputBuffer] No safe release delay found (limit {}), release unchanged", win);
        }

        if (chosen > 0 || !g_pending.empty())
            g_pending.push_back({chosen, false, button, isP1});
        else
            sendReal(false, button, isP1);

        g_activeDelay = 0;
    }

    void handleButton(bool down, int button, bool isP1) {
        auto pl = PlayLayer::get();
        if (g_passthrough || g_simulating || !pl || g_stage == 4 || !pl->m_isPracticeMode)
            return sendReal(down, button, isP1);

        if (!down) return judgeRelease(pl, button, isP1);
        judgeClick(pl, down, button, isP1);
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        if (g_simulating || isHalfTick) return;

        if (g_verify) verifyStep();

        if (g_pending.empty()) return;

        std::vector<Pending> due;
        for (auto& p : g_pending) p.ticks--;
        for (auto it = g_pending.begin(); it != g_pending.end();) {
            if (it->ticks <= 0) { due.push_back(*it); it = g_pending.erase(it); }
            else ++it;
        }
        g_passthrough = true;
        for (auto& p : due) sendReal(p.down, p.button, p.isP1);
        g_passthrough = false;
    }
};
