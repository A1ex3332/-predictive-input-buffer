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
    log::info("[InputBuffer] v1.0 LOADED");
    log::info("[InputBuffer] Starting Mode: {}", STAGES[g_stage].name);
    log::info("[InputBuffer] Press F6 in-level to cycle: 2/2 -> 4/4 -> 8/8 -> 16/8 -> OFF");
    log::info("[InputBuffer] ============================================");
}

// F6 cycles the stage
class $modify(PBKeys, CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool down, bool repeat, double timestamp) {
        if (down && !repeat && key == KEY_F6 && PlayLayer::get()) {
            g_stage = (g_stage + 1) % STAGE_COUNT;
            log::info("[InputBuffer] Mode -> {}", STAGES[g_stage].name);
        }
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat, timestamp);
    }
};

// Don't really die while simulating
class $modify(PBPlayLayer, PlayLayer) {
    struct Fields {
        std::deque<Ref<CheckpointObject>> history;
        int  pendingTicks = 0;   // 0 = nothing waiting
        bool pendingDown = false, pendingP1 = true;
        int  pendingBtn = 0;
    };

    void destroyPlayer(PlayerObject* p, GameObject* g) {
        if (g_simulating) { g_simDied = true; return; }
        PlayLayer::destroyPlayer(p, g);
    }
};

class $modify(PBGameLayer, GJBaseGameLayer) {
    static constexpr float TICK = 1.f / 240.f;

    void step() {
        GJBaseGameLayer::processCommands(TICK, false, true);
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        auto pl = PlayLayer::get();
        if (!pl || g_simulating || isHalfTick || !pl->m_isPracticeMode || g_stage == 4) return;

        auto f = static_cast<PBPlayLayer*>(pl)->m_fields.self();
        f->history.push_back(pl->createCheckpoint());
        while ((int)f->history.size() > 17) f->history.pop_front();

        // fire a delayed click
        if (f->pendingTicks > 0 && --f->pendingTicks == 0)
            GJBaseGameLayer::handleButton(f->pendingDown, f->pendingBtn, f->pendingP1);
    }

    bool survives(int ticks) {
        g_simulating = true; g_simDied = false;
        for (int i = 0; i < ticks && !g_simDied; i++) step();
        g_simulating = false;
        return !g_simDied;
    }

    void handleButton(bool down, int button, bool isP1) {
        auto pl = PlayLayer::get();
        if (!pl || g_simulating || !down || !pl->m_isPracticeMode || g_stage == 4)
            return GJBaseGameLayer::handleButton(down, button, isP1);

        auto player = isP1 ? pl->m_player1 : pl->m_player2;
        bool wave = player && player->m_isDart;
        int win  = wave ? STAGES[g_stage].wave : STAGES[g_stage].all;
        int look = Mod::get()->getSettingValue<int64_t>(wave ? "wave-lookahead" : "lookahead");
        bool fixLate = Mod::get()->getSettingValue<bool>("fix-late-clicks");

        auto f = static_cast<PBPlayLayer*>(pl)->m_fields.self();
        auto now = pl->createCheckpoint();

        for (int k = 0; k <= win; k++) {
            for (int sign : {+1, -1}) {
                if (k == 0 && sign == -1) continue;
                int shift = k * sign;

                if (shift >= 0) {
                    // press LATER by `shift` ticks
                    for (int i = 0; i < shift; i++) step();
                    GJBaseGameLayer::handleButton(down, button, isP1);
                    bool ok = survives(look);
                    pl->loadFromCheckpoint(now);
                    if (ok) {
                        if (shift == 0) return GJBaseGameLayer::handleButton(down, button, isP1);
                        log::info("[InputBuffer] Shifted input +{} ticks{}", shift, wave ? " (wave)" : "");
                        f->pendingTicks = shift; f->pendingDown = down;
                        f->pendingBtn = button;  f->pendingP1 = isP1;
                        return;
                    }
                } else {
                    // press EARLIER (rewind) - skipped unless "fix late clicks" is on
                    if (!fixLate) continue;
                    size_t idx = f->history.size() - 1 + shift;
                    if (idx >= f->history.size()) continue;
                    pl->loadFromCheckpoint(f->history[idx]);
                    GJBaseGameLayer::handleButton(down, button, isP1);
                    for (int i = 0; i < -shift; i++) step();
                    bool ok = survives(look);
                    pl->loadFromCheckpoint(now);
                    if (ok) {
                        log::info("[InputBuffer] Shifted input {} ticks{}", shift, wave ? " (wave)" : "");
                        pl->loadFromCheckpoint(f->history[idx]);
                        return GJBaseGameLayer::handleButton(down, button, isP1);
                    }
                }
            }
        }
        // nothing worked: let the click through unchanged
        GJBaseGameLayer::handleButton(down, button, isP1);
    }
};
