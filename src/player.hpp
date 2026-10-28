#pragma once
// Built-in macro player: plays xdBot / Mega Hack .gdr and Mega Hack .gdr2 files
// straight in the game, no converting. Playback follows xdBot's own logic so its
// macros run exactly as in xdBot: same frame counting (game time * 240 + 1),
// inputs applied right after each tick, and the per-frame position fixes that
// keep a run on track. The inputs go through the game's normal button handling,
// so Smart Click Sounds plays its clicks for them like for your own presses.
//
// Included once, from main.cpp.

#include <Geode/modify/GJBaseGameLayer.hpp>
#include "macro.hpp"

namespace mp {

static std::optional<scs::macro::Macro> g_macro;
static std::string g_loadedPath;
static std::filesystem::file_time_type g_loadedTime{};
static size_t g_input = 0, g_fix = 0;
static int g_prevFrame = -1;
static bool g_firstAttempt = true;
static bool g_macroInput = false;
static bool g_practiceWarned = false;
static CCLabelBMFont* g_label = nullptr;

static bool wanted() { return Mod::get()->getSettingValue<bool>("macro-play"); }
static bool active() { return wanted() && g_macro.has_value(); }

static std::filesystem::path filePath() {
	return Mod::get()->getSettingValue<std::filesystem::path>("macro-file");
}

static void notify(std::string const& text, NotificationIcon icon = NotificationIcon::Info) {
	Notification::create(text, icon)->show();
}

// (re)load the chosen file when it changed; returns false with a message on failure
static bool loadMacro(bool quiet = false) {
	auto path = filePath();
	std::error_code ec;
	if (path.empty() || !std::filesystem::is_regular_file(path, ec)) {
		g_macro.reset();
		g_loadedPath.clear();
		if (!quiet) notify("Choose a macro file in Smart Click Sounds' settings first", NotificationIcon::Warning);
		return false;
	}
	auto when = std::filesystem::last_write_time(path, ec);
	if (g_macro && g_loadedPath == path.string() && when == g_loadedTime) return true;
	std::string err;
	auto m = scs::macro::load(path, err);
	if (!m || m->inputs.empty()) {
		g_macro.reset();
		g_loadedPath.clear();
		notify("Couldn't load the macro: " + (m ? std::string("it has no inputs") : err), NotificationIcon::Error);
		return false;
	}
	g_macro = std::move(m);
	g_loadedPath = path.string();
	g_loadedTime = when;
	log::info("Macro loaded: {} ({} {}, {} presses, {} position fixes)", g_loadedPath, g_macro->format, g_macro->bot,
	          g_macro->presses(), g_macro->fixes.size());
	return true;
}

static int frameOf(PlayLayer* pl) {
	if (g_macro && g_macro->timeFrames) // xdBot: time-based frames
		return int(pl->m_gameState.m_levelTime * g_macro->framerate) + 1;
	return int(pl->m_gameState.m_currentProgress);
}

// jump to the right place in the macro for the current frame (start, respawn, checkpoint)
static void seek(int frame) {
	if (!g_macro) return;
	auto& in = g_macro->inputs;
	auto& fx = g_macro->fixes;
	g_input = size_t(std::lower_bound(in.begin(), in.end(), frame, [](auto const& a, int f) { return a.frame < f; }) - in.begin());
	g_fix = size_t(std::lower_bound(fx.begin(), fx.end(), frame, [](auto const& a, int f) { return a.frame < f; }) - fx.begin());
	g_prevFrame = -1;
}

static void updateLabel() {
	if (!g_label) return;
	bool on = active();
	g_label->setVisible(on && Mod::get()->getSettingValue<bool>("macro-label"));
	if (on) g_label->setString(("Macro: " + (g_macro->levelName.empty() ? std::string("playing") : g_macro->levelName)).c_str());
}

static void addLabel(PlayLayer* pl) {
	g_label = CCLabelBMFont::create("Macro", "bigFont.fnt");
	g_label->setID("scs-macro-label"_spr);
	g_label->setScale(0.3f);
	g_label->setOpacity(110);
	g_label->setAnchorPoint({0.f, 0.f});
	g_label->setPosition({4.f, 4.f});
	g_label->setZOrder(1000);
	pl->addChild(g_label);
	updateLabel();
}

// called when entering a level and when playback is switched on
static void start(PlayLayer* pl, bool restart) {
	g_firstAttempt = true;
	g_practiceWarned = false;
	if (!wanted()) { updateLabel(); return; }
	if (!loadMacro()) { updateLabel(); return; }
	updateLabel();
	if (!pl) return;
	if (restart) {
		pl->m_player1->releaseAllButtons();
		pl->m_player2->releaseAllButtons();
		pl->resetLevelFromStart();
	}
	seek(frameOf(pl));
}

static void stop(PlayLayer* pl) {
	updateLabel();
	if (!pl) return;
	pl->m_player1->releaseAllButtons();
	pl->m_player2->releaseAllButtons();
}

static bool flipControls(PlayLayer* pl) {
	return !pl->m_levelSettings->m_platformerMode && GameManager::get()->getGameVariable("0010");
}

} // namespace mp

class $modify(SCSMacroLayer, GJBaseGameLayer) {
	void handlePlaying(PlayLayer* pl, int frame) {
		auto& m = *mp::g_macro;
		if (pl->m_levelEndAnimationStarted) return;
		if (pl->m_player1->m_isDead) {
			pl->m_player1->releaseAllButtons();
			pl->m_player2->releaseAllButtons();
			return;
		}
		mp::g_macroInput = true;
		while (mp::g_input < m.inputs.size() && frame >= m.inputs[mp::g_input].frame) {
			auto const& in = m.inputs[mp::g_input++];
			// xdBot stores the game's own third argument; the GDR standard (Mega Hack)
			// stores "is player 2"
			bool arg = m.timeFrames ? in.player2 : !in.player2;
			if (m.timeFrames && mp::flipControls(pl)) arg = !arg;
			GJBaseGameLayer::handleButton(in.down, in.button, arg);
		}
		mp::g_macroInput = false;
		if (!Mod::get()->getSettingValue<bool>("macro-fixes")) return;
		while (mp::g_fix < m.fixes.size() && frame >= m.fixes[mp::g_fix].frame) {
			auto const& f = m.fixes[mp::g_fix++];
			if (f.p1.has && f.p1.x != 0.f && f.p1.y != 0.f) pl->m_player1->setPosition({f.p1.x, f.p1.y});
			if (f.p1.hasR && f.p1.r != 0.f) pl->m_player1->setRotation(f.p1.r);
			if (m_gameState.m_isDualMode) {
				if (f.p2.has && f.p2.x != 0.f && f.p2.y != 0.f) pl->m_player2->setPosition({f.p2.x, f.p2.y});
				if (f.p2.hasR && f.p2.r != 0.f) pl->m_player2->setRotation(f.p2.r);
			}
		}
	}

	void processCommands(float dt, bool isHalfTick, bool isLastTick) {
		auto pl = PlayLayer::get();
		if (!pl || static_cast<GJBaseGameLayer*>(pl) != this || !mp::active())
			return GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
		if (pl->m_isPracticeMode) { // checkpoints would break the run: don't play there
			if (!mp::g_practiceWarned) {
				mp::g_practiceWarned = true;
				mp::notify("The macro doesn't play in practice mode", NotificationIcon::Warning);
			}
			return GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
		}
		int frame = mp::frameOf(pl);
		// like xdBot: restart once at the start so the run begins from a clean state
		if (frame > 2 && mp::g_firstAttempt) {
			mp::g_firstAttempt = false;
			if (!pl->m_levelEndAnimationStarted) {
				pl->resetLevel();
				return;
			}
		}
		if (mp::g_prevFrame == frame && frame != 0) // one step per macro frame
			return GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
		GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
		frame = mp::frameOf(pl);
		if (mp::g_prevFrame >= 0 && frame < mp::g_prevFrame) mp::seek(frame); // time went back
		mp::g_prevFrame = frame;
		handlePlaying(pl, frame);
	}

	// while a macro plays, your own clicks don't interfere with the run
	void handleButton(bool down, int button, bool isPlayer1) {
		auto pl = PlayLayer::get();
		if (pl && static_cast<GJBaseGameLayer*>(pl) == this && mp::active() && !mp::g_macroInput && !pl->m_isPracticeMode &&
		    Mod::get()->getSettingValue<bool>("macro-ignore-input"))
			return;
		GJBaseGameLayer::handleButton(down, button, isPlayer1);
	}
};
