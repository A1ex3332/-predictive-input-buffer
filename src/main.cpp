// Basu Input Buffer v2.9 (cube-style wave, wave auto-release, orb alignment, full ghost state, drift check, in-game console on `)
//
// When you click (or release, in wave / robot), a hidden "ghost" player is simulated
// ahead with your input. If the ghost dies, the input is retried with 1..N ticks of
// delay and, if a delayed version clearly survives longer, your input is held back
// and delivered that many physics ticks later. 1 tick = 1/240 s = 0.25 frames @ 60fps.
//
// v2 changes:
//  - Bigger windows: F6 stages up to 32 ticks (8 frames) plus a CUSTOM stage (settings).
//  - Smart prediction: ghost uses orbs, pads, gravity portals and speed portals; mode /
//    size / dual / mirror / teleport portals end the prediction as "safe".
//  - Input queue: every input arriving during a delay is shifted by the same amount,
//    so taps and hold lengths are preserved (no more swallowed clicks / 1-tick taps).
//  - Time budget per click so big windows don't hitch.
//  - Fixes: ghost lifetime, init using `this`, time-warp dt, F6 only when GD focused,
//    dead flipGravity hook priority, optional console, in-game console (stage + live log, hidden by default).

#include <Geode/Geode.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/modify/EffectGameObject.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/GameObject.hpp>
#include <Geode/modify/HardStreak.hpp>
#include <Geode/modify/LevelEditorLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace geode::prelude;

namespace {
    constexpr int HOOK_PRIORITY = -1000000;
    constexpr float TICK = 0.25f;
    constexpr size_t PANEL_LINES = 8;
    constexpr int SPAM_STREAK = 3;  // this many fast inputs in a row = wave spam

    constexpr uint64_t typeBit(GameObjectType type) {
        return 1ull << static_cast<int>(type);
    }

    // Objects the ghost always collides with.
    constexpr uint64_t COLLIDING_TYPES = typeBit(GameObjectType::Solid) | typeBit(GameObjectType::Hazard) | typeBit(GameObjectType::Slope) |
                                         typeBit(GameObjectType::CollisionObject) | typeBit(GameObjectType::AnimatedHazard);

    // Objects the ghost may also interact with when smart prediction is on.
    constexpr uint64_t INTERACT_TYPES =
        typeBit(GameObjectType::YellowJumpPad) | typeBit(GameObjectType::PinkJumpPad) | typeBit(GameObjectType::RedJumpPad) |
        typeBit(GameObjectType::GravityPad) | typeBit(GameObjectType::SpiderPad) | typeBit(GameObjectType::YellowJumpRing) |
        typeBit(GameObjectType::PinkJumpRing) | typeBit(GameObjectType::RedJumpRing) | typeBit(GameObjectType::GravityRing) |
        typeBit(GameObjectType::GreenRing) | typeBit(GameObjectType::DropRing) | typeBit(GameObjectType::CustomRing) |
        typeBit(GameObjectType::DashRing) | typeBit(GameObjectType::GravityDashRing) | typeBit(GameObjectType::SpiderOrb) |
        typeBit(GameObjectType::InverseGravityPortal) | typeBit(GameObjectType::NormalGravityPortal) |
        typeBit(GameObjectType::GravityTogglePortal);

    // Portals that change too much state to simulate: reaching one ends prediction as "safe".
    constexpr uint64_t STOP_TYPES =
        typeBit(GameObjectType::ShipPortal) | typeBit(GameObjectType::CubePortal) | typeBit(GameObjectType::BallPortal) |
        typeBit(GameObjectType::UfoPortal) | typeBit(GameObjectType::WavePortal) | typeBit(GameObjectType::RobotPortal) |
        typeBit(GameObjectType::SpiderPortal) | typeBit(GameObjectType::SwingPortal) | typeBit(GameObjectType::DualPortal) |
        typeBit(GameObjectType::SoloPortal) | typeBit(GameObjectType::TeleportPortal) | typeBit(GameObjectType::TeleportOrb);

    // v2.8: size portals are applied to the ghost instead of ending the prediction.
    // (Mirror portals only flip the screen, so the ghost simply ignores them.)
    constexpr uint64_t SIZE_TYPES = typeBit(GameObjectType::RegularSizePortal) | typeBit(GameObjectType::MiniSizePortal);

    bool hasType(uint64_t mask, GameObject* object) {
        auto type = static_cast<unsigned>(object->m_objectType);
        return type < 64 && ((mask >> type) & 1);
    }

    float speedPortalValue(int id) {
        switch (id) {
            case 200: return 0.7f;   // slow
            case 201: return 0.9f;   // normal
            case 202: return 1.1f;   // fast
            case 203: return 1.3f;   // faster
            case 1334: return 1.6f;  // fastest
            default: return 0.f;
        }
    }

    struct Stage {
        bool enabled;
        int ticksAll;
        int ticksWave;
        bool custom;
    };

    constexpr Stage STAGES[] = {
        { true, 2, 2, false },
        { true, 4, 4, false },
        { true, 8, 8, false },
        { true, 16, 8, false },
        { true, 24, 12, false },
        { true, 32, 16, false },
        { true, 0, 0, true },
        { false, 0, 0, false },
    };
    constexpr int STAGE_COUNT = sizeof(STAGES) / sizeof(STAGES[0]);

    struct QueuedInput {
        int ticksLeft;
        bool down;
    };

    struct ObjSnapshot {
        GameObject* object;
        bool isActivated;
        EffectGameObject* effect;
        bool activated;
        bool byP1;
        bool byP2;
    };

    struct Buffer {
        PlayerObject* ghost1 = nullptr;
        PlayerObject* ghost2 = nullptr;
        CCLayer* ghostLayer = nullptr;

        CCNode* panel = nullptr;
        CCLabelBMFont* panelTitle = nullptr;               // current stage
        std::vector<CCLabelBMFont*> panelLines;            // one label per log line (colored)
        bool panelOpen = false;  // closed by default, remembered for the session
        std::deque<std::string> logLines;

        bool simulating = false;
        bool injecting = false;
        bool ghostDied = false;
        bool ghostHitPortal = false;
        bool realInput = false;
        bool appliedInput = false;
        bool isPlayer1 = true;

        bool enabled = true;
        int stage = 0;
        int ticksAll = 2;
        int ticksWave = 2;

        std::deque<QueuedInput> queue;
        int shift = 0;  // delay applied to inputs while the queue is active

        float tickLength = TICK;
        int sinceInput = 999;
        int rapid = 0;

        bool smart = true;
        bool console = false;
        double budgetMs = 6.0;
        bool practiceOnly = true;    // v2.1: only buffer in practice mode
        int groundHorizon = 130;     // v2.1: how far ahead cube/ball/spider predictions look
        int releaseShift = 0;        // v2.1: delay of the last delivered press, reused for its release
        int waveHorizon = 65;        // v2.2: how far ahead wave predictions look (mini = 15 less)
        bool assumeOrbs = true;      // v2.2: the ghost clicks every orb it touches ("calculation mode")
        bool held1 = false, held2 = false;   // button state the ghosts are meant to be in
        bool auto1 = false, auto2 = false;   // an orb auto-click is currently held on this ghost

        bool tryBothOrbs = true;     // v2.4: if a run touches an orb and dies, retry with the other orb choice
        bool orbClick = true;        // v2.4: orb choice of the current run
        bool touchedOrb = false;     // v2.4: the current run touched an orb
        bool skipPress = false;      // v2.4: run without pressing anything (auto-release hold check)
        bool avoidMovers = true;     // v2.4: don't buffer when the ghost's path has moving objects
        bool sawMover = false;       // v2.4: a run this decision touched a moving object
        bool trackMovers = false;    // v2.5.1: watch objects only in this run (not in every search run)
        bool autoRelease = false;    // v2.4: wave auto-release (practice mode only)
        int autoWindow = 6;          // v2.4: hold must die within this many ticks to auto-release
        bool autoReleased = false;   // v2.4: we released for you; your real release is swallowed
        int spamGap = 16;            // v2.5.2: inputs closer than this (ticks) count toward wave spam
        bool waveV46 = false;        // v2.7: use the old v4.6 wave rules instead of the cube-style ones
        bool orbAlign = true;        // v2.6: hold an early press back until the ghost is inside the orb
        int orbWindow = 16;          // v2.6: how far ahead (ticks) an orb may be for that
        int simTick = 0;             // v2.6: tick of the current run
        int firstOrbTick = -1;       // v2.6: first tick the current run touched an orb
        int orbUsedTick = -1;        // v2.6: first tick the current run actually used an orb
        bool driftCheck = false;     // v2.4: log how far the real player strays from the prediction
        std::vector<CCPoint>* recordPath = nullptr;  // v2.4: ghost1 positions of the current run

        std::unordered_set<GameObject*> simUsed;
        std::vector<ObjSnapshot> snapshots;
    } buf;

    std::string stageText();

    // v2.4 moving-object guard: last known position of each object the ghost collided with.
    struct Watched {
        double x;
        double y;
        int movedTick;
    };
    constexpr int MOVER_TICKS = 120;
    constexpr size_t WATCH_LIMIT = 4096;
    std::unordered_map<GameObject*, Watched> watched;

    // v2.4 drift check: the recorded ghost run for your last input and how far you strayed.
    struct Drift {
        std::vector<CCPoint> path;
        bool active = false;
        int index = -1;
        int compared = 0;
        float maxErr = 0.f;
        int maxAt = 0;
    } drift;

    constexpr float PANEL_W = 320.f;
    constexpr float PANEL_H = 178.f;

    // Log line colors: saves green, orbs cyan, auto-release purple, misses grey,
    // budget / skips orange, drift yellow.
    ccColor3B lineColor(std::string const& line) {
        auto starts = [&](char const* prefix) { return line.rfind(prefix, 0) == 0; };
        if (starts("SAVED")) return { 120, 255, 140 };
        if (starts("ORB")) return { 90, 220, 255 };
        if (starts("AUTO")) return { 200, 150, 255 };
        if (starts("no save")) return { 175, 175, 190 };
        if (starts("skip") || starts("prediction budget")) return { 255, 180, 80 };
        if (starts("drift")) return { 255, 240, 150 };
        if (starts("F6") || starts("v2")) return { 255, 215, 90 };
        return { 255, 255, 255 };
    }

    // In-game console (` key): current stage + the same live log the Windows console shows.
    // Hidden by default; only touched when open, so it costs nothing while closed.
    void refreshPanel() {
        if (!buf.panel) {
            return;
        }
        buf.panel->setVisible(buf.panelOpen);
        if (!buf.panelOpen) {
            return;
        }
        buf.panelTitle->setString(stageText().c_str());
        buf.panelTitle->limitLabelWidth(PANEL_W - 20.f, 0.4f, 0.2f);
        size_t first = buf.logLines.size() > buf.panelLines.size() ? buf.logLines.size() - buf.panelLines.size() : 0;
        for (size_t i = 0; i < buf.panelLines.size(); ++i) {
            auto* label = buf.panelLines[i];
            size_t at = first + i;
            if (at < buf.logLines.size()) {
                auto const& line = buf.logLines[at];
                label->setString(line.c_str());
                label->setColor(lineColor(line));
                label->limitLabelWidth(PANEL_W - 20.f, 0.55f, 0.3f);
            }
            else {
                label->setString("");
            }
        }
    }

    // Log line -> in-game panel history, plus the Windows console if enabled.
    // (Formatting a short string is cheap; it was the synchronous console writes
    //  that hitched, and those still only happen with the console setting on.)
    template <class... Args>
    void say(fmt::format_string<Args...> f, Args&&... args) {
        auto msg = fmt::format(f, std::forward<Args>(args)...);
        buf.logLines.push_back(msg);
        while (buf.logLines.size() > PANEL_LINES) {
            buf.logLines.pop_front();
        }
        if (buf.console) {
            std::printf("[InputBuffer] %s\n", msg.c_str());
        }
        if (buf.panelOpen) {
            refreshPanel();
        }
    }

    bool isGhost(PlayerObject* p) {
        return p && (p == buf.ghost1 || p == buf.ghost2);
    }

    void resetPending() {
        buf.queue.clear();
        buf.shift = 0;
        buf.releaseShift = 0;
    }

    void readSettings() {
        auto* mod = Mod::get();
        buf.smart = mod->getSettingValue<bool>("smart-prediction");
        buf.budgetMs = mod->getSettingValue<double>("sim-budget-ms");
        buf.practiceOnly = mod->getSettingValue<bool>("practice-only");
        buf.groundHorizon = static_cast<int>(mod->getSettingValue<int64_t>("ground-horizon"));
        buf.waveHorizon = static_cast<int>(mod->getSettingValue<int64_t>("wave-horizon"));
        buf.assumeOrbs = mod->getSettingValue<bool>("orb-assume-hit");
        buf.orbClick = buf.assumeOrbs;
        buf.tryBothOrbs = mod->getSettingValue<bool>("orb-try-both");
        buf.avoidMovers = mod->getSettingValue<bool>("avoid-moving-objects");
        buf.autoRelease = mod->getSettingValue<bool>("wave-auto-release");
        buf.autoWindow = static_cast<int>(mod->getSettingValue<int64_t>("auto-release-window"));
        buf.driftCheck = mod->getSettingValue<bool>("drift-check");
        buf.spamGap = static_cast<int>(mod->getSettingValue<int64_t>("wave-spam-gap"));
        buf.orbAlign = mod->getSettingValue<bool>("orb-align");
        buf.waveV46 = mod->getSettingValue<bool>("wave-v46");
        buf.orbWindow = static_cast<int>(mod->getSettingValue<int64_t>("orb-align-ticks"));
    }

    void applyStage() {
        auto const& s = STAGES[buf.stage];
        buf.enabled = s.enabled;
        if (s.custom) {
            buf.ticksAll = static_cast<int>(Mod::get()->getSettingValue<int64_t>("custom-ticks"));
            buf.ticksWave = static_cast<int>(Mod::get()->getSettingValue<int64_t>("custom-wave-ticks"));
        }
        else {
            buf.ticksAll = s.ticksAll;
            buf.ticksWave = s.ticksWave;
        }
    }

    std::string stageText() {
        if (!buf.enabled) {
            return "Buffer: OFF";
        }
        return fmt::format("Buffer{}: {:g}f / wave {:g}f", STAGES[buf.stage].custom ? " (custom)" : "", buf.ticksAll * TICK,
            buf.ticksWave * TICK);
    }

    void dropGhosts() {
        for (auto** g : { &buf.ghost1, &buf.ghost2 }) {
            if (*g) {
                (*g)->removeFromParentAndCleanup(true);
                (*g)->release();
                *g = nullptr;
            }
        }
        if (buf.ghostLayer) {
            buf.ghostLayer->removeAllChildrenWithCleanup(true);
            buf.ghostLayer->release();
            buf.ghostLayer = nullptr;
        }
        // panel nodes are owned by the PlayLayer
        buf.panel = nullptr;
        buf.panelTitle = nullptr;
        buf.panelLines.clear();
        buf.simulating = false;
        buf.injecting = false;
        buf.rapid = 0;
        buf.autoReleased = false;
        buf.recordPath = nullptr;
        watched.clear();
        drift.active = false;
        resetPending();
    }

    // PERF: v1 created ghosts with the PlayLayer as their parent layer and put them in
    // the object layer. PlayerObject::init adds ~13 particle systems, 2 motion streaks,
    // a wave trail and a ghost trail to that parent layer, so every frame the game was
    // updating and drawing two invisible players' worth of effects (which also kept
    // emitting wherever the ghost was last simulated). Ghosts now live in a detached
    // layer that is never added to the scene: cocos never visits/draws it and its
    // nodes' scheduled updates stay paused, so the ghosts cost nothing between clicks.
    PlayerObject* makeGhost(PlayLayer* layer) {
        if (!buf.ghostLayer) {
            buf.ghostLayer = CCLayer::create();
            buf.ghostLayer->retain();
        }
        auto* ghost = PlayerObject::create(1, 1, layer, buf.ghostLayer, true);
        ghost->retain();
        ghost->setPosition(CCPoint(0.f, 105.f));
        ghost->setVisible(false);
        buf.ghostLayer->addChild(ghost);
        return ghost;
    }

    void sendButton(PlayLayer* layer, bool down) {
        buf.injecting = true;
        buf.appliedInput = down;
        layer->handleButton(down, 1, buf.isPlayer1);
        buf.injecting = false;
    }

    void pressGhost(PlayerObject* ghost, bool down) {
        (ghost == buf.ghost1 ? buf.held1 : buf.held2) = down;
        if (down) {
            ghost->pushButton(PlayerButton::Jump);
        }
        else {
            ghost->releaseButton(PlayerButton::Jump);
        }
    }

    void copyState(PlayerObject* ghost, PlayerObject* real) {
        ghost->setVisible(false);
        ghost->resetTouchedRings(true);
        ghost->copyAttributes(real);
        ghost->setScale(real->getScale());
        ghost->setContentSize(real->getContentSize());
        ghost->m_width = real->m_width;
        ghost->m_height = real->m_height;
        ghost->m_gravityMod = real->m_gravityMod;
        ghost->m_isOnGround = real->m_isOnGround;
        ghost->m_isUpsideDown = real->m_isUpsideDown;
        ghost->m_vehicleSize = real->m_vehicleSize;
        ghost->m_playerSpeed = real->m_playerSpeed;
        ghost->m_isDashing = real->m_isDashing;
        ghost->m_lastActivatedPortal = real->m_lastActivatedPortal;

        // Button / jump-buffer state. Without this the ghost keeps whatever button
        // state the PREVIOUS simulation left it in (usually "held"), so on some clicks
        // its pushButton() is a no-op or it auto-jumps from hold, and the prediction
        // flips between right and wrong from click to click.
        ghost->m_holdingButtons = real->m_holdingButtons;
        ghost->m_jumpBuffered = real->m_jumpBuffered;
        ghost->m_wasJumpBuffered = real->m_wasJumpBuffered;
        ghost->m_stateJumpBuffered = real->m_stateJumpBuffered;
        ghost->m_stateRingJump = real->m_stateRingJump;
        ghost->m_stateRingJump2 = real->m_stateRingJump2;
        ghost->m_wasRobotJump = real->m_wasRobotJump;
        ghost->m_touchedRing = real->m_touchedRing;
        ghost->m_touchedPad = real->m_touchedPad;
        ghost->m_touchedGravityPortal = real->m_touchedGravityPortal;

        // v2.8: everything else the physics step reads, so every mode matches the real
        // player: cube / ball / ufo / wave / robot / spider / swing, mini, upside down,
        // reverse (going left), sideways, dual. copyAttributes() doesn't carry most of
        // it, the slope state above all (wave and cube riding slopes drifted without it).
        ghost->m_isShip = real->m_isShip;
        ghost->m_isBird = real->m_isBird;
        ghost->m_isBall = real->m_isBall;
        ghost->m_isDart = real->m_isDart;
        ghost->m_isRobot = real->m_isRobot;
        ghost->m_isSpider = real->m_isSpider;
        ghost->m_isSwing = real->m_isSwing;
        ghost->m_isGoingLeft = real->m_isGoingLeft;
        ghost->m_isSideways = real->m_isSideways;
        ghost->m_reverseSync = real->m_reverseSync;
        ghost->m_reverseRelated = real->m_reverseRelated;
        ghost->m_maybeReverseSpeed = real->m_maybeReverseSpeed;
        ghost->m_maybeReverseAcceleration = real->m_maybeReverseAcceleration;
        ghost->m_speedMultiplier = real->m_speedMultiplier;
        ghost->m_gravity = real->m_gravity;
        ghost->m_yStart = real->m_yStart;
        ghost->m_fixGravityBug = real->m_fixGravityBug;
        ghost->m_fixRobotJump = real->m_fixRobotJump;
        ghost->m_enable22Changes = real->m_enable22Changes;
        ghost->m_enableImpulseFix = real->m_enableImpulseFix;
        ghost->m_totalTime = real->m_totalTime;

        // vertical motion
        ghost->m_yVelocity = real->m_yVelocity;
        ghost->m_fallSpeed = real->m_fallSpeed;
        ghost->m_yVelocityBeforeSlope = real->m_yVelocityBeforeSlope;
        ghost->m_yVelocityRelated = real->m_yVelocityRelated;
        ghost->m_yVelocityRelated3 = real->m_yVelocityRelated3;
        ghost->m_groundYVelocity = real->m_groundYVelocity;
        ghost->m_accelerationOrSpeed = real->m_accelerationOrSpeed;
        ghost->m_isAccelerating = real->m_isAccelerating;
        ghost->m_maybeIsFalling = real->m_maybeIsFalling;
        ghost->m_isOnGround2 = real->m_isOnGround2;
        ghost->m_isOnGround3 = real->m_isOnGround3;
        ghost->m_isOnGround4 = real->m_isOnGround4;
        ghost->m_stateOnGround = real->m_stateOnGround;
        ghost->m_lastLandTime = real->m_lastLandTime;
        ghost->m_lastJumpTime = real->m_lastJumpTime;
        ghost->m_lastFlipTime = real->m_lastFlipTime;
        ghost->m_lastSpiderFlipTime = real->m_lastSpiderFlipTime;
        ghost->m_lastGroundedPos = real->m_lastGroundedPos;
        ghost->m_blackOrbRelated = real->m_blackOrbRelated;
        ghost->m_padRingRelated = real->m_padRingRelated;
        ghost->m_ringJumpRelated = real->m_ringJumpRelated;

        // slopes
        ghost->m_isOnSlope = real->m_isOnSlope;
        ghost->m_wasOnSlope = real->m_wasOnSlope;
        ghost->m_slopeVelocity = real->m_slopeVelocity;
        ghost->m_maybeUpsideDownSlope = real->m_maybeUpsideDownSlope;
        ghost->m_currentSlope = real->m_currentSlope;
        ghost->m_currentSlope2 = real->m_currentSlope2;
        ghost->m_currentPotentialSlope = real->m_currentPotentialSlope;
        ghost->m_isCollidingWithSlope = real->m_isCollidingWithSlope;
        ghost->m_collidingWithSlopeId = real->m_collidingWithSlopeId;
        ghost->m_slopeFlipGravityRelated = real->m_slopeFlipGravityRelated;
        ghost->m_slopeAngle = real->m_slopeAngle;
        ghost->m_slopeAngleRadians = real->m_slopeAngleRadians;
        ghost->m_slopeRotation = real->m_slopeRotation;
        ghost->m_currentSlopeYVelocity = real->m_currentSlopeYVelocity;
        ghost->m_isCurrentSlopeTop = real->m_isCurrentSlopeTop;
        ghost->m_slopeStartTime = real->m_slopeStartTime;
        ghost->m_slopeEndTime = real->m_slopeEndTime;
        ghost->m_slopeSlidingMaybeRotated = real->m_slopeSlidingMaybeRotated;
        ghost->m_stateDartSlide = real->m_stateDartSlide;

        // what it's standing on / touching
        ghost->m_lastGroundObject = real->m_lastGroundObject;
        ghost->m_preLastGroundObject = real->m_preLastGroundObject;
        ghost->m_maybeLastGroundObject = real->m_maybeLastGroundObject;
        ghost->m_collidedObject = real->m_collidedObject;
        ghost->m_objectSnappedTo = real->m_objectSnappedTo;
        ghost->m_snapDistance = real->m_snapDistance;
        ghost->m_lastCollisionBottom = real->m_lastCollisionBottom;
        ghost->m_lastCollisionTop = real->m_lastCollisionTop;
        ghost->m_lastCollisionLeft = real->m_lastCollisionLeft;
        ghost->m_lastCollisionRight = real->m_lastCollisionRight;
        ghost->m_collidedTopMinY = real->m_collidedTopMinY;
        ghost->m_collidedBottomMaxY = real->m_collidedBottomMaxY;
        ghost->m_collidedLeftMaxX = real->m_collidedLeftMaxX;
        ghost->m_collidedRightMinX = real->m_collidedRightMinX;

        // dash orbs
        ghost->m_dashX = real->m_dashX;
        ghost->m_dashY = real->m_dashY;
        ghost->m_dashAngle = real->m_dashAngle;
        ghost->m_dashStartTime = real->m_dashStartTime;
        ghost->m_dashRing = real->m_dashRing;
        ghost->m_dashFireFrame = real->m_dashFireFrame;

        // force / boost / state triggers already acting on the player
        ghost->m_stateForce = real->m_stateForce;
        ghost->m_stateForceVector = real->m_stateForceVector;
        ghost->m_affectedByForces = real->m_affectedByForces;
        ghost->m_stateBoostX = real->m_stateBoostX;
        ghost->m_stateBoostY = real->m_stateBoostY;
        ghost->m_stateNoStickX = real->m_stateNoStickX;
        ghost->m_stateNoStickY = real->m_stateNoStickY;
        ghost->m_stateHitHead = real->m_stateHitHead;
        ghost->m_stateFlipGravity = real->m_stateFlipGravity;
        ghost->m_stateNoAutoJump = real->m_stateNoAutoJump;
        ghost->m_stateScale = real->m_stateScale;
        ghost->m_maybeStateForce2 = real->m_maybeStateForce2;
    }

    // Your recent input timings, so predictions model what you actually do instead of
    // an infinite hold: cube taps (holding auto-re-jumps) and robot holds (they set the
    // jump height).
    constexpr int HOLD_SAMPLES = 8;

    struct HoldStats {
        int samples[HOLD_SAMPLES] = {};
        int count = 0;
        int index = 0;

        void add(int ticks) {
            samples[index] = std::clamp(ticks, 1, 240);
            index = (index + 1) % HOLD_SAMPLES;
            count = std::min(count + 1, HOLD_SAMPLES);
        }

        // percentile of the recent samples, or `fallback` with no samples yet
        int pick(int fallback, int percent) const {
            if (count == 0) {
                return fallback;
            }
            int tmp[HOLD_SAMPLES];
            std::copy(samples, samples + count, tmp);
            int at = std::min(count - 1, count * percent / 100);
            std::nth_element(tmp, tmp + at, tmp + count);
            return tmp[at];
        }
    };

    HoldStats tapHold, robotHold;
    int tickCount = 0;

    enum class InputKind { Tap, Wave, Robot, Other };
    int pressTick = -1;
    InputKind pressKind = InputKind::Other;

    // Cube / ball / ufo / spider / swing are tapped. Ship, wave and robot are held, and
    // each keeps its own samples so long holds don't pollute the cube's tap length.
    InputKind kindOf(PlayerObject* p) {
        if (!p || p->m_isShip) {
            return InputKind::Other;
        }
        if (p->m_isDart) {
            return InputKind::Wave;
        }
        return p->m_isRobot ? InputKind::Robot : InputKind::Tap;
    }

    void recordRealInput(bool down, PlayerObject* p1) {
        if (down) {
            pressTick = tickCount;
            pressKind = kindOf(p1);
            return;
        }
        if (pressTick < 0) {
            return;
        }
        int held = tickCount - pressTick;
        pressTick = -1;
        if (buf.autoReleased) {
            return;  // the game already let go earlier; this length isn't a real hold
        }
        switch (pressKind) {
            case InputKind::Tap: tapHold.add(held); break;
            case InputKind::Robot: robotHold.add(held); break;
            default: break;
        }
    }

    int holdEstimate() {
        return tapHold.pick(12, 50);  // ~50 ms, a typical tap
    }

    // v2.4 moving-object guard: objects the ghost collided with are watched every tick,
    // and any that moved recently make the decision "don't buffer" (the ghost sees the
    // level frozen at the click, so it tests them in the wrong place).
    void watchObject(GameObject* object) {
        auto [it, added] = watched.try_emplace(object, Watched { object->m_positionX, object->m_positionY, -MOVER_TICKS });
        if (!added && tickCount - it->second.movedTick < MOVER_TICKS) {
            buf.sawMover = true;
        }
    }

    void updateWatched() {
        if (watched.size() > WATCH_LIMIT) {
            watched.clear();
        }
        for (auto& [object, w] : watched) {
            if (object->m_positionX != w.x || object->m_positionY != w.y) {
                w.x = object->m_positionX;
                w.y = object->m_positionY;
                w.movedTick = tickCount;
            }
        }
    }

    // v2.4 drift check: the ghost run for the input you actually made is recorded, then
    // compared tick by tick with where the real player goes, until your next input.
    void finishDrift(char const* why) {
        if (!drift.active) {
            return;
        }
        drift.active = false;
        if (drift.compared >= 8) {
            say("drift {:.1f}u max at t{} over {}t{}", drift.maxErr, drift.maxAt, drift.compared, why);
        }
    }

    void driftStep(PlayerObject* real) {
        if (!drift.active) {
            return;
        }
        if (drift.index >= 0) {
            if (drift.index >= static_cast<int>(drift.path.size())) {
                finishDrift("");
                return;
            }
            float err = ccpDistance(real->m_position, drift.path[drift.index]);
            if (err > drift.maxErr) {
                drift.maxErr = err;
                drift.maxAt = drift.index;
            }
            ++drift.compared;
        }
        ++drift.index;
    }

    void snapshotObject(GameObject* object) {
        if (!object || buf.simUsed.contains(object)) {
            return;
        }
        buf.simUsed.insert(object);
        ObjSnapshot s { object, object->m_isActivated, nullptr, false, false, false };
        if (auto* effect = typeinfo_cast<EffectGameObject*>(object)) {
            s.effect = effect;
            s.activated = effect->m_activated;
            s.byP1 = effect->m_activatedByPlayer1;
            s.byP2 = effect->m_activatedByPlayer2;
        }
        buf.snapshots.push_back(s);
    }

    void restoreObjects() {
        for (auto const& s : buf.snapshots) {
            s.object->m_isActivated = s.isActivated;
            if (s.effect) {
                s.effect->m_activated = s.activated;
                s.effect->m_activatedByPlayer1 = s.byP1;
                s.effect->m_activatedByPlayer2 = s.byP2;
            }
        }
        buf.snapshots.clear();
        buf.simUsed.clear();
    }

    void advanceGhost(PlayerObject* ghost, float dt) {
        ghost->update(dt);
        ghost->updateRotation(dt);
    }

    void checkGhost(PlayLayer* layer, PlayerObject* ghost, float dt) {
        ghost->m_collisionLogTop->removeAllObjects();
        ghost->m_collisionLogBottom->removeAllObjects();
        ghost->m_collisionLogLeft->removeAllObjects();
        ghost->m_collisionLogRight->removeAllObjects();
        layer->checkCollisions(ghost, dt, false);
    }

    bool isFlying(PlayerObject* player) {
        return player->m_isDart || player->m_isBird || player->m_isSwing;
    }

    // Orb auto-clicks last one ghost update, then let go (unless the test itself is holding).
    void releaseAutoClicks() {
        if (buf.auto1) {
            if (!buf.held1 && buf.ghost1) {
                buf.ghost1->releaseButton(PlayerButton::Jump);
            }
            buf.auto1 = false;
        }
        if (buf.auto2) {
            if (!buf.held2 && buf.ghost2) {
                buf.ghost2->releaseButton(PlayerButton::Jump);
            }
            buf.auto2 = false;
        }
    }

    enum class SimEnd { Ok, StopSearch };

    // Returns ticks survived after the input (horizon = fully safe).
    // releaseAfter: ticks after the input at which the ghost does the opposite (-1 = never).
    // stopAt: stop as soon as this many ticks were survived (the caller already knows
    // that's good enough), instead of always running the full horizon.
    int simulate(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, int delay, bool down, int horizon, int releaseAfter,
        int stopAt, SimEnd* end) {
        *end = SimEnd::Ok;
        if (!buf.ghost1) {
            return horizon;
        }
        buf.simulating = true;
        buf.ghostDied = false;
        buf.ghostHitPortal = false;
        buf.firstOrbTick = -1;
        buf.orbUsedTick = -1;

        bool second = dual && buf.ghost2 && p2;
        copyState(buf.ghost1, p1);
        if (second) {
            copyState(buf.ghost2, p2);
        }
        buf.held1 = buf.held2 = buf.appliedInput;
        buf.auto1 = buf.auto2 = false;

        // v4.6 wave: before a delayed input fires, the ghost is in the opposite state
        // (holding before a delayed release, released before a delayed press).
        bool waveRun = p1->m_isDart || (second && p2->m_isDart);
        if (buf.waveV46 && waveRun && delay > 0 && !buf.skipPress) {
            pressGhost(buf.ghost1, !down);
            if (second) {
                pressGhost(buf.ghost2, !down);
            }
        }

        float dt = buf.tickLength;
        if (!(dt > 0.001f && dt < 2.0f)) {
            dt = TICK;
        }

        // (v1 forced a fake !down button event here for delay > 0, which made delayed
        //  runs start from a different jump-buffer state than the "press now" run.
        //  The ghost now simply mirrors the real player's button state, except in wave,
        //  which keeps the v4.6 behaviour above.)

        bool flying = isFlying(p1) || (dual && p2 && isFlying(p2));
        int total = delay + horizon;
        int survived = 0;
        int airTicks = 0;
        int groundTicks = 0;
        int tick = 0;
        bool died = false;
        bool safe = false;

        for (; tick < total; ++tick) {
            buf.simTick = tick;
            if (tick == delay && !buf.skipPress) {
                pressGhost(buf.ghost1, down);
                if (second) {
                    pressGhost(buf.ghost2, down);
                }
            }
            if (releaseAfter >= 0 && tick == delay + releaseAfter) {
                // let go of a press (or, for a wave release, press again: the turn back)
                pressGhost(buf.ghost1, !down);
                if (second) {
                    pressGhost(buf.ghost2, !down);
                }
            }

            advanceGhost(buf.ghost1, dt);
            if (second) {
                advanceGhost(buf.ghost2, dt);
            }
            releaseAutoClicks();
            if (buf.ghostDied) {
                died = true;
                break;
            }

            checkGhost(layer, buf.ghost1, dt);
            if (!buf.ghostDied && second) {
                checkGhost(layer, buf.ghost2, dt);
            }
            if (buf.ghostDied) {
                died = true;
                break;
            }
            if (buf.recordPath) {
                buf.recordPath->push_back(buf.ghost1->m_position);
            }
            if (buf.ghostHitPortal) {
                safe = tick >= delay;
                break;
            }

            if (tick < delay) {
                continue;
            }
            ++survived;
            if (survived >= stopAt) {
                break;
            }
            if (flying) {
                continue;
            }

            bool grounded = buf.ghost1->m_isOnGround && (!second || buf.ghost2->m_isOnGround);
            if (!grounded) {
                ++airTicks;
                groundTicks = 0;
                continue;
            }
            if (airTicks < 6) {
                continue;
            }
            if (groundTicks > 2) {
                safe = true;
                break;
            }
            ++groundTicks;
        }

        // Died or hit an unsimulated portal before the delayed input even fired:
        // longer delays can't do better, stop searching.
        if ((died || buf.ghostHitPortal) && tick < delay) {
            *end = SimEnd::StopSearch;
        }
        restoreObjects();
        buf.simulating = false;
        return safe ? horizon : survived;
    }

    // v2.4: simulate() with the orb setting; if that run touched an orb and didn't make
    // it, also try the other choice (click it / skip it) and keep whichever lives longer,
    // like a player who only clicks the orb when they need it.
    int simulateOrbs(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, int delay, bool down, int horizon, int releaseAfter,
        int stopAt, SimEnd* end) {
        buf.orbClick = buf.assumeOrbs;
        buf.touchedOrb = false;
        int first = simulate(layer, p1, p2, dual, delay, down, horizon, releaseAfter, stopAt, end);
        if (!buf.tryBothOrbs || !buf.smart || !buf.touchedOrb || first >= stopAt) {
            return first;
        }
        SimEnd firstEnd = *end;
        buf.orbClick = !buf.assumeOrbs;
        int second = simulate(layer, p1, p2, dual, delay, down, horizon, releaseAfter, stopAt, end);
        buf.orbClick = buf.assumeOrbs;
        if (firstEnd != SimEnd::StopSearch) {
            *end = SimEnd::Ok;
        }
        return std::max(first, second);
    }

    // v2.4: record the ghost run for the input that was actually made (drift check).
    void startDrift(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, int delay, bool down, int horizon, int releaseAfter) {
        finishDrift("");
        if (!buf.driftCheck) {
            return;
        }
        drift.path.clear();
        buf.recordPath = &drift.path;
        buf.orbClick = buf.assumeOrbs;
        SimEnd end;
        simulate(layer, p1, p2, dual, delay, down, horizon, releaseAfter, delay + horizon, &end);
        buf.recordPath = nullptr;
        drift.active = !drift.path.empty();
        drift.index = -1;
        drift.compared = 0;
        drift.maxErr = 0.f;
        drift.maxAt = 0;
    }

    // Full wave look-ahead from the setting (mini = 15 less).
    int fullWaveHorizon(bool mini) {
        int wh = std::clamp(buf.waveHorizon, 20, 160);
        return mini ? std::max(20, wh - 15) : wh;  // v4.6: 65 normal / 50 mini
    }

    // v2.6 orb alignment: a press made a little before reaching an orb would be wasted
    // (or jump off the ground instead). Find the delay that lands the press inside the
    // orb, like the cube buffer does for jumps. Returns that delay, or 0.
    int alignOrb(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, int horizon, int releaseAfter, int* landed) {
        int window = std::clamp(buf.orbWindow, 1, 64);
        int found = 0;
        SimEnd end;
        buf.orbClick = false;  // the press being tested is the orb click

        // 1. Cheap probe: without pressing, does the ghost reach an orb soon?
        buf.skipPress = true;
        simulate(layer, p1, p2, dual, 0, true, window + 2, -1, window + 2, &end);
        buf.skipPress = false;
        int touch = buf.firstOrbTick;

        if (touch >= 0 && touch <= window) {
            // 2. Pressing now already uses an orb (e.g. held into it): nothing to align.
            int now = simulate(layer, p1, p2, dual, 0, true, horizon, releaseAfter, horizon, &end);
            if (buf.orbUsedTick < 0) {
                // 3. Earliest delay around the touch tick whose press really uses the orb,
                //    and that doesn't crash sooner than missing the orb would.
                for (int k = std::max(1, touch - 1); k <= std::min(window, touch + 3); ++k) {
                    int ret = simulate(layer, p1, p2, dual, k, true, horizon, releaseAfter, horizon, &end);
                    if (end == SimEnd::StopSearch) {
                        break;
                    }
                    if (buf.orbUsedTick >= 0 && ret >= std::min(now, horizon)) {
                        *landed = ret;
                        found = k;
                        break;
                    }
                }
            }
        }
        buf.orbClick = buf.assumeOrbs;
        return found;
    }

    // v2.1: best delayed run seen by the last search (shown in the "no save" log line)
    int dbgBestK = 0;
    int dbgBestRet = -1;

    // v2.7 wave scoring, the wave version of the cube's "landed safely". A wave never
    // lands, so a run that keeps going one way eventually hits a wall even in a normal
    // corridor, where you'd simply turn. So: if the run crashes, try turning back a bit
    // before the crash; if that turn survives, you had time to react and the run counts
    // as safe. Only crashes you couldn't have turned away from count as deaths.
    constexpr int WAVE_TURN_MARGIN = 6;   // turn this many ticks before the crash
    constexpr int WAVE_MIN_TURN = 6;      // a turn sooner than this after the input is too fast to count
    constexpr int WAVE_ESCAPE = 16;       // the turn must then survive this many ticks

    int waveScore(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, int delay, bool down, int horizon, SimEnd* end) {
        int ret = simulateOrbs(layer, p1, p2, dual, delay, down, horizon, -1, horizon, end);
        if (ret >= horizon || *end == SimEnd::StopSearch) {
            return ret;
        }
        int turnAt = ret - WAVE_TURN_MARGIN;
        if (turnAt < WAVE_MIN_TURN) {
            return ret;
        }
        int length = std::min(horizon, turnAt + WAVE_ESCAPE);
        SimEnd turnEnd;
        int turned = simulateOrbs(layer, p1, p2, dual, delay, down, length, turnAt, length, &turnEnd);
        return turned >= length ? horizon : ret;
    }

    // v2.7 wave search, same rules as the cube: the smallest delay that is safe (or
    // clearly better than now) wins, so turns are only held back as long as needed.
    int searchWave(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, bool down, int horizon, int maxDelay, int base,
        int* landed) {
        using clock = std::chrono::steady_clock;
        auto start = clock::now();
        int target = std::min(horizon, base + 12);
        dbgBestK = 0;
        dbgBestRet = -1;
        for (int k = 1; k <= maxDelay; ++k) {
            SimEnd end;
            int ret = waveScore(layer, p1, p2, dual, k, down, horizon, &end);
            if (ret > dbgBestRet) {
                dbgBestRet = ret;
                dbgBestK = k;
            }
            if (end == SimEnd::StopSearch) {
                return 0;
            }
            if (ret >= target) {
                *landed = ret;
                return k;
            }
            if (std::chrono::duration<double, std::milli>(clock::now() - start).count() > buf.budgetMs) {
                say("prediction budget hit at +{} ticks", k);
                return 0;
            }
        }
        return 0;
    }

    // Find the smallest delay that clearly beats pressing now. 0 = don't buffer.
    int searchDelay(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, bool down, bool anyWave, int horizon, int maxDelay,
        int releaseAfter, int base, int* landed) {
        using clock = std::chrono::steady_clock;
        auto start = clock::now();
        auto overBudget = [&] {
            return std::chrono::duration<double, std::milli>(clock::now() - start).count() > buf.budgetMs;
        };

        int target = std::min(horizon, base + 12);
        dbgBestK = 0;
        dbgBestRet = -1;
        int waveBestK = 0;
        int waveBestRet = base;
        for (int k = 1; k <= maxDelay; ++k) {
            SimEnd end;
            // Wave needs the full run to compare gains; elsewhere reaching `target` is enough.
            int ret = simulateOrbs(layer, p1, p2, dual, k, down, horizon, releaseAfter, anyWave ? horizon : target, &end);
            if (ret > dbgBestRet) {
                dbgBestRet = ret;
                dbgBestK = k;
            }
            if (end == SimEnd::StopSearch) {
                break;
            }
            if (anyWave) {
                // v4.6 wave rule: delaying a turn in a flat corridor naturally adds ~2 ticks
                // per tick of delay against the opposite wall, so only buffer when the delayed
                // run clearly clears a spike, saw or corner. A fully safe run needs
                // 2*k+6 extra ticks and ends the search; otherwise keep the best run that
                // gains at least 2*k+10 and beats the previous best by more than 2.
                if (ret >= horizon && ret - base >= k * 2 + 6) {
                    waveBestK = k;
                    waveBestRet = ret;
                    break;
                }
                if (ret >= base + k * 2 + 10 && ret > waveBestRet + 2) {
                    waveBestK = k;
                    waveBestRet = ret;
                    if (ret >= horizon) {
                        break;
                    }
                }
            }
            else if (ret >= target) {
                *landed = ret;
                return k;
            }
            if (overBudget()) {
                say("prediction budget hit at +{} ticks", k);
                break;
            }
        }
        if (anyWave && waveBestK > 0) {
            *landed = waveBestRet;
            return waveBestK;
        }
        return 0;
    }

    void queueInput(int ticks, bool down) {
        buf.queue.push_back({ ticks, down });
    }

    bool intercept(GJBaseGameLayer* self, bool down, bool isPlayer1) {
        auto* layer = GameManager::get()->m_playLayer;
        if (!layer) {
            return false;
        }

        bool dual = layer->m_gameState.m_isDualMode && layer->m_player2;
        auto* level = layer->m_level;
        if (level && !isPlayer1 && level->m_twoPlayerMode) {
            return false;
        }

        buf.isPlayer1 = isPlayer1;
        buf.realInput = down;
        if (buf.autoReleased) {
            // the game already let go for you: your real release has nothing left to do
            buf.autoReleased = false;
            if (!down) {
                return true;
            }
        }
        if (!buf.enabled) {
            buf.appliedInput = down;
            return false;
        }
        if (buf.practiceOnly && !layer->m_isPracticeMode) {
            buf.appliedInput = down;
            return false;
        }
        // v2.4: the ghost never presses left/right, so platformer predictions are meaningless.
        if (layer->m_isPlatformer) {
            buf.appliedInput = down;
            return false;
        }

        auto* p1 = self->m_player1;
        auto* p2 = dual ? layer->m_player2 : nullptr;
        if (!p1 || p1->m_isDead || p1->m_isShip || (p2 && p2->m_isShip)) {
            resetPending();
            buf.appliedInput = down;
            return false;
        }

        if (down && buf.queue.empty()) {
            buf.releaseShift = 0;
        }

        bool p1Wave = p1->m_isDart;
        bool anyWave = p1Wave || (p2 && p2->m_isDart);
        bool mini = (p1Wave && p1->m_vehicleSize < 1.f) || (p2 && p2->m_isDart && p2->m_vehicleSize < 1.f);

        // Wave spam: don't fight fast inputs. Counted for every input, including ones made
        // while a delayed input is waiting (v2.5.1 skipped those, so a spam that started
        // with a buffered click never reached the streak and its taps got collapsed).
        // Once the streak is reached, anything still waiting is dropped and your inputs
        // go straight to the game until you pause for longer than the spam gap.
        if (buf.sinceInput < std::clamp(buf.spamGap, 4, 40)) {
            ++buf.rapid;
        }
        else {
            buf.rapid = 0;
        }
        buf.sinceInput = 0;
        if (anyWave && buf.rapid >= SPAM_STREAK) {
            resetPending();
            buf.appliedInput = down;
            return false;
        }

        // v4.6 wave mode only: while a delayed input waits, a quick opposite input fires
        // one tick after it; pressing the same way again cancels that. (The default wave
        // mode shifts it like the cube does, below, so your zigzag keeps its shape.)
        if (!buf.queue.empty() && anyWave && buf.waveV46) {
            auto first = buf.queue.front();
            buf.queue.erase(buf.queue.begin() + 1, buf.queue.end());
            if (down != first.down) {
                queueInput(first.ticksLeft + 1, down);
            }
            return true;
        }

        // A delay is in progress: shift this input by the same amount so your
        // rhythm (tap length, hold length, follow-up clicks) is preserved.
        if (!buf.queue.empty()) {
            int after = buf.queue.back().ticksLeft;
            queueInput(std::max(buf.shift, after), down);
            return true;
        }

        // Releases only matter for wave and robot.
        if (!p1Wave) {
            bool robot = p1->m_isRobot;
            bool wave2 = false;
            if (p2 && !robot) {
                wave2 = p2->m_isDart;
                if (!wave2) {
                    robot = p2->m_isRobot;
                }
            }
            if (!wave2 && !down && !robot) {
                if (buf.releaseShift > 0) {
                    // the press was delayed, so its release is delayed by the same amount:
                    // the hold length the ghost was tested with is the one the game gets
                    buf.shift = buf.releaseShift;
                    buf.releaseShift = 0;
                    queueInput(buf.shift, false);
                    return true;
                }
                buf.appliedInput = false;
                return false;
            }
        }

        bool anyFlyer = isFlying(p1) || (p2 && isFlying(p2));  // wave / ufo / swing
        int horizon = anyFlyer ? 48 : std::clamp(buf.groundHorizon, 48, 240);
        if (anyWave) {
            horizon = fullWaveHorizon(mini);
        }
        int maxDelay = anyWave ? buf.ticksWave : buf.ticksAll;
        if (maxDelay <= 0) {
            buf.appliedInput = down;
            return false;
        }

        // Wave releases are predicted separately, so hold there. Robot jump height depends
        // on the hold, so use your usual robot hold (once known); everything else (cube,
        // ball, ufo, spider, swing) re-triggers while held, so model a tap.
        bool anyRobot = p1->m_isRobot || (p2 && p2->m_isRobot);
        int releaseAfter = -1;
        if (down && !anyWave) {
            releaseAfter = anyRobot ? robotHold.pick(-1, 50) : holdEstimate();
        }

        if (down && !anyWave && buf.orbAlign && buf.smart) {
            int orbLanded = 0;
            int k = alignOrb(layer, p1, p2, dual, horizon, releaseAfter, &orbLanded);
            if (k > 0) {
                say("ORB press +{}t ({:g}f) -> {}t", k, k * TICK, orbLanded);
                startDrift(layer, p1, p2, dual, k, down, horizon, releaseAfter);
                buf.shift = k;
                queueInput(k, down);
                return true;
            }
        }

        buf.sawMover = false;
        buf.trackMovers = buf.avoidMovers;
        SimEnd end;
        bool cubeStyleWave = anyWave && !buf.waveV46;
        int base = cubeStyleWave ? waveScore(layer, p1, p2, dual, 0, down, horizon, &end)
                                 : simulateOrbs(layer, p1, p2, dual, 0, down, horizon, releaseAfter, horizon, &end);
        buf.trackMovers = false;
        if (base >= horizon) {
            startDrift(layer, p1, p2, dual, 0, down, horizon, releaseAfter);
            buf.appliedInput = down;
            return false;
        }

        int landed = 0;
        int saved = cubeStyleWave ? searchWave(layer, p1, p2, dual, down, horizon, maxDelay, base, &landed)
                                  : searchDelay(layer, p1, p2, dual, down, anyWave, horizon, maxDelay, releaseAfter, base, &landed);
        if (saved <= 0) {
            say("no save: {} dies in {}t (max {}t, best +{}t -> {}t of {})", down ? "press" : "release", base, maxDelay,
                dbgBestK, dbgBestRet, horizon);
            startDrift(layer, p1, p2, dual, 0, down, horizon, releaseAfter);
            buf.appliedInput = down;
            return false;
        }
        if (buf.avoidMovers) {
            // One extra run of the chosen delay with the moving-object watch on. (Watching
            // in every search run made cube searches hit the time budget.)
            buf.trackMovers = true;
            simulateOrbs(layer, p1, p2, dual, saved, down, horizon, releaseAfter, horizon, &end);
            buf.trackMovers = false;
        }
        if (buf.avoidMovers && buf.sawMover) {
            say("skip +{}t save: moving objects ahead", saved);
            startDrift(layer, p1, p2, dual, 0, down, horizon, releaseAfter);
            buf.appliedInput = down;
            return false;
        }

        say("SAVED {} +{}t ({:g}f)  {}->{}  [max {}]", down ? "press" : "release", saved, saved * TICK, base, landed, maxDelay);
        startDrift(layer, p1, p2, dual, saved, down, horizon, releaseAfter);
        buf.shift = saved;
        queueInput(saved, down);
        return true;
    }

    void cycleStage() {
        buf.stage = (buf.stage + 1) % STAGE_COUNT;
        resetPending();
        applyStage();
        readSettings();
        say("F6 -> {}", stageText());
        refreshPanel();
    }

    // v2.4 wave auto-release (practice mode only): while you hold in wave, check every
    // tick whether holding crashes within the next `autoWindow` ticks. If it does and
    // letting go now clearly survives longer, let go for you. Your own release later is
    // swallowed, and your next press works as normal.
    void tryAutoRelease(PlayLayer* layer, PlayerObject* p1) {
        if (!buf.autoRelease || buf.autoReleased || !layer->m_isPracticeMode || layer->m_isPlatformer) {
            return;
        }
        if (!buf.queue.empty() || !buf.realInput || !buf.appliedInput || !buf.isPlayer1 || !buf.ghost1) {
            return;
        }
        if (layer->m_level && layer->m_level->m_twoPlayerMode) {
            return;
        }
        bool dual = layer->m_gameState.m_isDualMode && layer->m_player2;
        auto* p2 = dual ? layer->m_player2 : nullptr;
        if (p1->m_isDead || !(p1->m_isDart || (p2 && p2->m_isDart))) {
            return;
        }
        bool mini = (p1->m_isDart && p1->m_vehicleSize < 1.f) || (p2 && p2->m_isDart && p2->m_vehicleSize < 1.f);

        int window = std::clamp(buf.autoWindow, 2, 24);
        buf.sawMover = false;
        buf.skipPress = true;
        buf.orbClick = buf.assumeOrbs;
        SimEnd end;
        int hold = simulate(layer, p1, p2, dual, 0, true, window, -1, window, &end);
        buf.skipPress = false;
        if (hold >= window || buf.ghostHitPortal) {
            return;
        }

        int horizon = fullWaveHorizon(mini);
        buf.trackMovers = buf.avoidMovers;
        int release = simulateOrbs(layer, p1, p2, dual, 0, false, horizon, -1, horizon, &end);
        buf.trackMovers = false;
        if (release < horizon && release < hold + 10) {
            return;
        }
        if (buf.avoidMovers && buf.sawMover) {
            return;
        }
        buf.autoReleased = true;
        sendButton(layer, false);
        say("AUTO release: holding dies in {}t, letting go lives {}t", hold, release);
    }

    void deliverPending(PlayLayer* layer, PlayerObject* player) {
        if (buf.sinceInput <= 998) {
            ++buf.sinceInput;
        }
        if (buf.avoidMovers && !watched.empty()) {
            updateWatched();
        }

        bool dual = layer->m_gameState.m_isDualMode && layer->m_player2;
        if (player->m_isShip || (dual && layer->m_player2->m_isShip)) {
            resetPending();
            buf.autoReleased = false;
            if (buf.appliedInput != buf.realInput) {
                sendButton(layer, buf.realInput);
            }
            return;
        }
        // Left wave while auto-released: the game goes back to what you're really holding.
        if (buf.autoReleased && !(player->m_isDart || (dual && layer->m_player2->m_isDart))) {
            buf.autoReleased = false;
        }

        if (buf.queue.empty()) {
            if (buf.appliedInput != buf.realInput && !buf.autoReleased) {
                sendButton(layer, buf.realInput);
            }
            tryAutoRelease(layer, player);
            return;
        }

        // Check before decrementing: an input queued with N ticks fires right before
        // physics step N, which is exactly what simulate(delay = N) tested.
        while (!buf.queue.empty() && buf.queue.front().ticksLeft <= 0) {
            bool down = buf.queue.front().down;
            buf.queue.pop_front();
            buf.releaseShift = down ? buf.shift : 0;
            if (down != buf.appliedInput) {
                sendButton(layer, down);
            }
        }
        for (auto& q : buf.queue) {
            --q.ticksLeft;
        }
        if (buf.queue.empty()) {
            buf.shift = 0;
        }
    }
}

$on_mod(Loaded) {
    buf.console = Mod::get()->getSettingValue<bool>("debug-console");
    readSettings();
    applyStage();
    if (buf.console) {
        AllocConsole();
        FILE* out = nullptr;
        freopen_s(&out, "CONOUT$", "w", stdout);
        SetConsoleTitleA("Basu Input Buffer - Live Console");
    }
    say("v2.9 loaded. F6 cycles: 0.5f -> 1f -> 2f -> 4f -> 6f -> 8f -> custom -> off");
}

class $modify(BufPlayLayer, PlayLayer) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("PlayLayer::destroyPlayer", HOOK_PRIORITY);
        (void)self.setHookPriority("PlayLayer::playEndAnimationToPos", HOOK_PRIORITY);
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (buf.simulating && isGhost(player)) {
            buf.ghostDied = true;
            return;
        }
        resetPending();
        buf.appliedInput = false;
        buf.autoReleased = false;
        buf.rapid = 0;
        finishDrift(" (died)");
        PlayLayer::destroyPlayer(player, object);
    }

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) {
            return false;
        }
        dropGhosts();
        readSettings();
        applyStage();
        buf.ghost1 = makeGhost(this);
        buf.ghost2 = makeGhost(this);

        // In-game console (` key): hidden unless the player opens it.
        constexpr float W = PANEL_W;
        constexpr float H = PANEL_H;
        auto win = CCDirector::get()->getWinSize();
        buf.panel = CCNode::create();
        buf.panel->setPosition({ 8.f, win.height - H - 8.f });
        buf.panel->setZOrder(1000);

        // Background: the game's own blue background art, cropped to the panel and
        // darkened from top to bottom so the text stays readable.
        auto* stencil = CCLayerColor::create({ 255, 255, 255, 255 }, W, H);
        auto* clip = CCClippingNode::create(stencil);
        if (auto* art = CCSprite::create("game_bg_01_001.png")) {
            auto size = art->getContentSize();
            art->setScale(std::max(W / size.width, H / size.height));
            art->setPosition({ W / 2.f, H / 2.f });
            art->setColor({ 40, 120, 255 });
            clip->addChild(art);
        }
        auto* shade = CCLayerGradient::create({ 0, 0, 0, 110 }, { 0, 0, 0, 215 }, { 0.f, -1.f });
        shade->setContentSize({ W, H });
        clip->addChild(shade);
        auto* header = CCLayerColor::create({ 0, 0, 0, 120 }, W, 28.f);
        header->setPosition({ 0.f, H - 28.f });
        clip->addChild(header);
        auto* rule = CCLayerColor::create({ 255, 255, 255, 45 }, W - 16.f, 1.f);
        rule->setPosition({ 8.f, H - 46.f });
        clip->addChild(rule);
        buf.panel->addChild(clip);

        if (auto* frame = CCScale9Sprite::create("GJ_square07.png")) {
            frame->setContentSize({ W, H });
            frame->setPosition({ W / 2.f, H / 2.f });
            buf.panel->addChild(frame);
        }

        auto* title = CCLabelBMFont::create("Basu Input Buffer", "goldFont.fnt");
        title->setScale(0.55f);
        title->setAnchorPoint({ 0.f, 0.5f });
        title->setPosition({ 10.f, H - 14.f });
        buf.panel->addChild(title);

        auto* version = CCLabelBMFont::create(Mod::get()->getVersion().toVString().c_str(), "chatFont.fnt");
        version->setScale(0.45f);
        version->setOpacity(150);
        version->setAnchorPoint({ 1.f, 0.5f });
        version->setPosition({ W - 10.f, H - 14.f });
        buf.panel->addChild(version);

        buf.panelTitle = CCLabelBMFont::create("", "bigFont.fnt");
        buf.panelTitle->setScale(0.4f);
        buf.panelTitle->setAnchorPoint({ 0.f, 0.5f });
        buf.panelTitle->setPosition({ 10.f, H - 37.f });
        buf.panel->addChild(buf.panelTitle);

        buf.panelLines.clear();
        for (size_t i = 0; i < PANEL_LINES; ++i) {
            auto* line = CCLabelBMFont::create("", "chatFont.fnt");
            line->setScale(0.55f);
            line->setAnchorPoint({ 0.f, 0.5f });
            line->setPosition({ 10.f, H - 57.f - i * 13.f });
            buf.panel->addChild(line);
            buf.panelLines.push_back(line);
        }

        auto* hint = CCLabelBMFont::create("F6  change stage      `  hide console", "chatFont.fnt");
        hint->setScale(0.45f);
        hint->setColor({ 160, 190, 255 });
        hint->setAnchorPoint({ 0.5f, 0.5f });
        hint->setPosition({ W / 2.f, 9.f });
        buf.panel->addChild(hint);

        this->addChild(buf.panel);
        refreshPanel();

        buf.realInput = false;
        buf.appliedInput = false;
        buf.isPlayer1 = true;
        buf.sinceInput = 999;
        buf.rapid = 0;
        return true;
    }

    void onQuit() {
        dropGhosts();
        PlayLayer::onQuit();
    }

    void resetLevel() {
        resetPending();
        buf.autoReleased = false;
        drift.active = false;
        PlayLayer::resetLevel();
    }

    void playEndAnimationToPos(CCPoint position) {
        if (buf.simulating) {
            return;
        }
        PlayLayer::playEndAnimationToPos(position);
    }
};

class $modify(BufEditorLayer, LevelEditorLayer) {
    bool init(GJGameLevel* level, bool noUI) {
        bool ok = LevelEditorLayer::init(level, noUI);
        dropGhosts();
        return ok;
    }
};

class $modify(BufHardStreak, HardStreak) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("HardStreak::addPoint", HOOK_PRIORITY);
    }

    void addPoint(CCPoint point) {
        if (buf.simulating) {
            return;
        }
        HardStreak::addPoint(point);
    }
};

class $modify(BufEffectGameObject, EffectGameObject) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("EffectGameObject::triggerObject", HOOK_PRIORITY);
    }

    void triggerObject(GJBaseGameLayer* layer, int uniqueID, gd::vector<int> const* remapKeys) {
        if (buf.simulating) {
            return;
        }
        EffectGameObject::triggerObject(layer, uniqueID, remapKeys);
    }
};

class $modify(BufGameObject, GameObject) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("GameObject::playShineEffect", HOOK_PRIORITY);
    }

    void playShineEffect() {
        if (buf.simulating) {
            return;
        }
        GameObject::playShineEffect();
    }
};

class $modify(BufPlayerObject, PlayerObject) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("PlayerObject::playSpiderDashEffect", HOOK_PRIORITY);
        (void)self.setHookPriority("PlayerObject::incrementJumps", HOOK_PRIORITY);
        (void)self.setHookPriority("PlayerObject::ringJump", HOOK_PRIORITY);
    }

    // Runs 240x/sec per player. Ghost steps (during a simulation) skip straight to
    // the original; F6 is no longer polled here (see BufKeyboard below).
    void update(float dt) {
        if (buf.simulating) {
            PlayerObject::update(dt);
            return;
        }
        auto* layer = GameManager::get()->m_playLayer;
        if (layer && this == layer->m_player1) {
            ++tickCount;
            driftStep(this);
            if (buf.enabled) {
                deliverPending(layer, this);
            }
        }

        PlayerObject::update(dt);

        // The ghost must step with exactly the dt the real player gets.
        if (dt > 0.f) {
            buf.tickLength = dt;
        }
    }

    void incrementJumps() {
        if (buf.simulating) {
            return;
        }
        PlayerObject::incrementJumps();
    }

    void playSpiderDashEffect(CCPoint from, CCPoint to) {
        if (buf.simulating) {
            return;
        }
        PlayerObject::playSpiderDashEffect(from, to);
    }

    void ringJump(RingObject* ring, bool skipCheck) {
        if (buf.simulating) {
            if (!buf.smart || !isGhost(this)) {
                return;
            }
            snapshotObject(ring);
            if (buf.orbUsedTick < 0) {
                buf.orbUsedTick = buf.simTick;
            }
        }
        PlayerObject::ringJump(ring, skipCheck);
    }

    // Dash orbs don't go through ringJump.
    void startDashing(DashRingObject* ring) {
        if (buf.simulating && isGhost(this) && buf.orbUsedTick < 0) {
            buf.orbUsedTick = buf.simTick;
        }
        PlayerObject::startDashing(ring);
    }
};

// F6 via the game's own keyboard events: no polling, and it only fires while GD has
// focus (v1 polled GetAsyncKeyState every physics tick, even when tabbed out).
class $modify(BufKeyboard, CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool isKeyDown, bool isKeyRepeat, double timestamp) {
        if (isKeyDown && !isKeyRepeat && GameManager::get()->m_playLayer) {
            if (key == KEY_F6) {
                cycleStage();
            }
            else if (key == KEY_OEM3) {  // ` (backtick / tilde key)
                buf.panelOpen = !buf.panelOpen;
                refreshPanel();
            }
        }
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, isKeyDown, isKeyRepeat, timestamp);
    }
};

class $modify(BufBaseGameLayer, GJBaseGameLayer) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("GJBaseGameLayer::canBeActivatedByPlayer", HOOK_PRIORITY);
        (void)self.setHookPriority("GJBaseGameLayer::handleButton", HOOK_PRIORITY);
        (void)self.setHookPriority("GJBaseGameLayer::collisionCheckObjects", HOOK_PRIORITY);
        (void)self.setHookPriority("GJBaseGameLayer::playerTouchedRing", HOOK_PRIORITY);
        (void)self.setHookPriority("GJBaseGameLayer::flipGravity", HOOK_PRIORITY);
        (void)self.setHookPriority("GJBaseGameLayer::gameEventTriggered", HOOK_PRIORITY);
        (void)self.setHookPriority("GJBaseGameLayer::playerWillSwitchMode", HOOK_PRIORITY);
    }

    // During a simulation the ghost may use each object once; the real activation
    // state is snapshotted and restored afterwards so the real player is unaffected.
    bool canBeActivatedByPlayer(PlayerObject* player, EffectGameObject* object) {
        if (buf.simulating) {
            if (!buf.smart || !isGhost(player) || !object || buf.simUsed.contains(object)) {
                return false;
            }
            snapshotObject(object);
            return true;
        }
        return GJBaseGameLayer::canBeActivatedByPlayer(player, object);
    }

    void collisionCheckObjects(PlayerObject* player, gd::vector<GameObject*>* objects, int objectCount, float dt) {
        if (!buf.simulating) {
            GJBaseGameLayer::collisionCheckObjects(player, objects, objectCount, dt);
            return;
        }
        static gd::vector<GameObject*> relevant;
        relevant.clear();
        if (static_cast<int>(relevant.capacity()) < objectCount) {
            relevant.reserve(objectCount);
        }

        // Hot loop: runs for every nearby object, every ghost tick. One type lookup per
        // object, no bounds-checked at(), and the ghost rect is only fetched if needed.
        uint64_t keep = buf.smart ? (COLLIDING_TYPES | INTERACT_TYPES) : COLLIDING_TYPES;
        bool haveRect = false;
        CCRect ghostRect;
        auto& list = *objects;
        for (int i = 0; i < objectCount; ++i) {
            auto* object = list[i];
            if (!object) {
                continue;
            }
            auto type = static_cast<unsigned>(object->m_objectType);
            uint64_t bit = type < 64 ? (1ull << type) : 0;
            if (keep & bit) {
                relevant.push_back(object);
                if (buf.trackMovers && (COLLIDING_TYPES & bit)) {
                    watchObject(object);
                }
                continue;
            }
            if (!buf.smart) {
                continue;
            }
            bool stop = (STOP_TYPES & bit) != 0;
            bool size = (SIZE_TYPES & bit) != 0;
            float speed = (stop || size) ? 0.f : speedPortalValue(object->m_objectID);
            if (!stop && !size && speed <= 0.f) {
                continue;
            }
            if (!haveRect) {
                ghostRect = player->getObjectRect();
                haveRect = true;
            }
            if (ghostRect.intersectsRect(object->getObjectRect())) {
                if (stop) {
                    buf.ghostHitPortal = true;
                }
                else if (size) {
                    bool mini = object->m_objectType == GameObjectType::MiniSizePortal;
                    if ((player->m_vehicleSize < 1.f) != mini) {
                        player->togglePlayerScale(mini, true);
                    }
                }
                else {
                    player->m_playerSpeed = speed;
                }
            }
        }
        GJBaseGameLayer::collisionCheckObjects(player, &relevant, static_cast<int>(relevant.size()), dt);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        if (!buf.simulating && button == 1 && !buf.injecting) {
            auto* p1 = this->m_player1;
            if (down || (p1 && (p1->m_isDart || p1->m_isRobot))) {
                finishDrift("");
            }
            recordRealInput(down, p1);
        }
        if (!buf.simulating && button == 1 && !buf.injecting && intercept(this, down, isPlayer1)) {
            return;
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    void playerTouchedRing(PlayerObject* player, RingObject* ring) {
        if (buf.simulating) {
            if (!buf.smart || !isGhost(player)) {
                return;
            }
            snapshotObject(ring);
            buf.touchedOrb = true;
            if (buf.firstOrbTick < 0) {
                buf.firstOrbTick = buf.simTick;
            }
        }
        GJBaseGameLayer::playerTouchedRing(player, ring);

        // v2.2 calculation mode: assume you click every orb you touch, on time. Without
        // this the ghost (which only has the one click being tested) misses the next orb in a
        // chain, falls, and the mod thinks a perfectly good click is a death.
        if (buf.simulating && buf.orbClick) {
            bool first = player == buf.ghost1;
            bool& held = first ? buf.held1 : buf.held2;
            bool& autoClick = first ? buf.auto1 : buf.auto2;
            if (!held && !autoClick) {
                player->pushButton(PlayerButton::Jump);
                autoClick = true;
            }
        }
    }

    // Gravity portals / blue pads / blue orbs: flip only the ghost, no layer effects.
    void flipGravity(PlayerObject* player, bool flip, bool noEffects) {
        if (buf.simulating) {
            if (buf.smart && isGhost(player)) {
                player->flipGravity(flip, true);
            }
            return;
        }
        GJBaseGameLayer::flipGravity(player, flip, noEffects);
    }

    // Never let ghost activity fire event triggers in the real level.
    void gameEventTriggered(GJGameEvent event, int material, int playerID) {
        if (buf.simulating) {
            return;
        }
        GJBaseGameLayer::gameEventTriggered(event, material, playerID);
    }

    // Safety net in case a mode portal slips through: end prediction, change nothing.
    void playerWillSwitchMode(PlayerObject* player, GameObject* object) {
        if (buf.simulating) {
            buf.ghostHitPortal = true;
            return;
        }
        GJBaseGameLayer::playerWillSwitchMode(player, object);
    }
};
