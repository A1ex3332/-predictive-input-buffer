#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <deque>

using namespace geode::prelude;

// ---------- stages ----------
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

static void logBanner() {
    log::info("[InputBuffer] ============================================");
    log::info("[InputBuffer] v1.0 LOADED (Wave Spikes/Saws Trajectory Fix)");
    log::info("[InputBuffer] Starting Mode: {}", STAGES[g_stage].name);
    log::info("[InputBuffer] Press F6 in-level to cycle: 2/2 -> 4/4 -> 8/8 -> 16/8 -> OFF");
    log::info("[InputBuffer] ============================================");
}

$on_mod(Loaded) { logBanner(); }

// ---------- F6 cycles stages ----------
class $modify(PBKeys, CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool down, bool repeat) {
        if (down && !repeat && key == KEY_F6 && PlayLayer::get()) {
            g_stage = (g_stage + 1) % STAGE_COUNT;
            log::info("[InputBuffer] Mode -> {}", STAGES[g_stage].name);
        }
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat);
    }
};

// ---------- play layer: block real death while simulating ----------
class $modify(PBPlayLayer, PlayLayer) {
    struct Fields {
        std::deque<Ref<CheckpointObject>> history;
        int  pendingTicks = -1;
        bool pendingDown = false, pendingP1 = true;
        int  pendingBtn = 0;
    };

    void destroyPlayer(PlayerObject* p, GameObject* g) {
        if (g_simulating) { g_simDied = true; return; }
        PlayLayer::destroyPlayer(p, g);
    }
};

// ---------- core: snapshot + shift search ----------
class $modify(PBGameLayer, GJBaseGameLayer) {
    static constexpr float TICK = 1.f / 240.f; // verify the real step

    void processCommands(float dt) {
        GJBaseGameLayer::processCommands(dt);
        auto pl = PlayLayer::get();
        if (!pl || g_simulating || !pl->m_isPracticeMode || g_stage == 4) return;

        auto f = static_cast<PBPlayLayer*>(pl)->m_fields.self();
        f->history.push_back(pl->createCheckpoint());
        while ((int)f->history.size() > 17) f->history.pop_front();

        if (f->pendingTicks >= 0 && --f->pendingTicks < 0)
            GJBaseGameLayer::handleButton(f->pendingDown, f->pendingBtn, f->pendingP1);
    }

    bool survives(int ticks) {
        g_simulating = true; g_simDied = false;
        for (int i = 0; i < ticks && !g_simDied; i++)
            GJBaseGameLayer::processCommands(TICK);
        g_simulating = false;
        return !g_simDied;
    }

    void handleButton(bool down, int button, bool isP1) {
        auto pl = PlayLayer::get();
        if (!pl || g_simulating || !down || !pl->m_isPracticeMode || g_stage == 4)
            return GJBaseGameLayer::handleButton(down, button, isP1);

        // wave uses its own window and a longer lookahead
        auto player = isP1 ? pl->m_player1 : pl->m_player2;
        bool wave = player && player->m_isDart;
        int win  = wave ? STAGES[g_stage].wave : STAGES[g_stage].all;
        int look = Mod::get()->getSettingValue<int64_t>(wave ? "wave-lookahead" : "lookahead");

        auto f = static_cast<PBPlayLayer*>(pl)->m_fields.self();
        auto now = pl->createCheckpoint();

        for (int k = 0; k <= win; k++) {
            for (int sign : {+1, -1}) {
                if (k == 0 && sign == -1) continue;
                int shift = k * sign;

                if (shift >= 0) {                       // press LATER
                    for (int i = 0; i < shift; i++) GJBaseGameLayer::processCommands(TICK);
                    GJBaseGameLayer::handleButton(down, button, isP1);
                    bool ok = survives(look);
                    pl->loadFromCheckpoint(now);
                    if (ok) {
                        if (shift > 0) log::info("[InputBuffer] Shifted input +{} ticks{}", shift, wave ? " (wave)" : "");
                        f->pendingTicks = shift; f->pendingDown = down;
                        f->pendingBtn = button;  f->pendingP1 = isP1;
                        if (shift == 0) { f->pendingTicks = -1; return GJBaseGameLayer::handleButton(down, button, isP1); }
                        return;
                    }
                } else {                                // press EARLIER (rewind)
                    size_t idx = f->history.size() - 1 + shift;
                    if (idx >= f->history.size()) continue;
                    pl->loadFromCheckpoint(f->history[idx]);
                    GJBaseGameLayer::handleButton(down, button, isP1);
                    for (int i = 0; i < -shift; i++) GJBaseGameLayer::processCommands(TICK);
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
        // nothing in the window survives: pass the original input through
        GJBaseGameLayer::handleButton(down, button, isP1);
    }
};
