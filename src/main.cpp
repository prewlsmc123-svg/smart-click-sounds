// Smart Click Sounds: plays real click sounds in-game on every jump press and
// release. engine.hpp decides which click variation and how loud (based on how
// long your finger was up / held), voices.hpp builds that variation's audio
// (from the recorded clicks, tone-cleaned), and this file plays it through GD's
// FMOD. Because the game itself plays the sounds, they're always in sync with
// whatever drives the inputs: you, a macro, practice mode, speedhack.

#include <Geode/Geode.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include "voices.hpp"

#ifdef GEODE_IS_WINDOWS
#include <ShlObj.h>
#endif

using namespace geode::prelude;
namespace fs = std::filesystem;

static scs::Engine g_engine;
static scs::Pack g_pack;
static std::string g_loadedKey = "\x01";
static FMOD::ChannelGroup* g_group = nullptr;
static FMOD::Channel* g_last[2][2] = {};
struct Live { FMOD::Sound* sound; FMOD::Channel* channel; };
static std::vector<Live> g_live; // sounds we created; released once they finish
static FMOD::Sound* g_roomSound = nullptr;
static FMOD::Channel* g_roomChannel = nullptr;

// ---------- pack loading ----------

static fs::path documentsPack() {
#ifdef GEODE_IS_WINDOWS
	PWSTR p = nullptr;
	if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &p))) {
		fs::path d(p);
		CoTaskMemFree(p);
		return d / "GDClickbot" / "clickpack";
	}
#endif
	return {};
}

// decode a PCM (8/16/24/32-bit) or float WAV to mono 48 kHz floats
static scs::Buf readWav(fs::path const& path) {
	std::ifstream f(path, std::ios::binary);
	std::vector<char> d((std::istreambuf_iterator<char>(f)), {});
	scs::Buf x;
	if (d.size() < 44 || std::string(d.data(), 4) != "RIFF") return x;
	auto u16 = [&](size_t o) { return uint16_t(uint8_t(d[o]) | uint8_t(d[o + 1]) << 8); };
	auto u32 = [&](size_t o) { return uint32_t(u16(o) | uint32_t(u16(o + 2)) << 16); };
	size_t pos = 12, dataOff = 0, dataLen = 0;
	int fmt = 1, ch = 1, bits = 16, rate = scs::kRate;
	while (pos + 8 <= d.size()) {
		std::string id(d.data() + pos, 4);
		size_t sz = u32(pos + 4), body = pos + 8;
		if (id == "fmt " && body + 16 <= d.size()) {
			fmt = u16(body); ch = std::max<int>(1, u16(body + 2)); rate = int(u32(body + 4)); bits = u16(body + 14);
			if (fmt == 0xFFFE && body + 26 <= d.size()) fmt = u16(body + 24);
		} else if (id == "data") {
			dataOff = body; dataLen = std::min(sz, d.size() - body);
		}
		pos = body + sz + (sz & 1);
	}
	int bps = bits / 8;
	if (!dataOff || bps == 0) return x;
	size_t n = dataLen / (size_t(bps) * ch);
	x.resize(n);
	for (size_t i = 0; i < n; i++) {
		float s = 0;
		for (int c = 0; c < ch; c++) {
			const char* p = d.data() + dataOff + (i * ch + c) * bps;
			if (fmt == 1 && bits == 8) s += (uint8_t(p[0]) - 128) / 128.f;
			else if (fmt == 1 && bits == 16) s += int16_t(uint8_t(p[0]) | uint8_t(p[1]) << 8) / 32768.f;
			else if (fmt == 1 && bits == 24) s += (int32_t((uint8_t(p[0]) << 8) | (uint8_t(p[1]) << 16) | (uint8_t(p[2]) << 24)) >> 8) / 8388608.f;
			else if (fmt == 1 && bits == 32) s += float(int32_t(uint8_t(p[0]) | uint8_t(p[1]) << 8 | uint8_t(p[2]) << 16 | uint32_t(uint8_t(p[3])) << 24) / 2147483648.0);
			else if (fmt == 3 && bits == 32) { float v; std::memcpy(&v, p, 4); s += v; }
		}
		x[i] = s / ch;
	}
	if (rate != scs::kRate && rate > 0 && !x.empty()) { // resample to 48 kHz
		double r = double(rate) / scs::kRate;
		scs::Buf y(size_t(x.size() / r));
		for (size_t i = 0; i < y.size(); i++) {
			double t = i * r; size_t j = size_t(t); float fr = float(t - j);
			y[i] = j + 1 < x.size() ? x[j] * (1 - fr) + x[j + 1] * fr : x.back();
		}
		x.swap(y);
	}
	return x;
}

static std::string trailingNumber(std::string name) {
	auto dot = name.rfind('.');
	if (dot != std::string::npos) name = name.substr(0, dot);
	size_t i = name.size();
	while (i > 0 && std::isdigit(uint8_t(name[i - 1]))) i--;
	return name.substr(i);
}

static void loadDir(fs::path const& dir, std::string const& prefix, int list, std::vector<std::string>& nums, bool clean) {
	std::error_code ec;
	if (!fs::is_directory(dir, ec)) return;
	std::vector<fs::path> files;
	for (auto& e : fs::directory_iterator(dir, ec)) {
		if (!e.is_regular_file()) continue;
		auto ext = e.path().extension().string();
		for (auto& c : ext) c = char(std::tolower(uint8_t(c)));
		auto name = e.path().filename().string();
		if (ext == ".wav" && (prefix.empty() || name.rfind(prefix, 0) == 0)) files.push_back(e.path());
	}
	std::sort(files.begin(), files.end());
	auto& out = g_pack.list(g_engine, list);
	for (auto& f : files) {
		scs::Buf x = readWav(f);
		if (x.size() < 100) continue;
		if (clean) scs::cleanTone(x);
		out.push_back({nullptr, scs::loudnessDB(x.data(), x.size(), scs::kRate), -1});
		g_pack.pcm[list].push_back(std::move(x));
		nums.push_back(trailingNumber(f.filename().string()));
	}
}

static void pairUp(std::vector<scs::Sound>& presses, std::vector<std::string> const& pn, std::vector<std::string> const& rn) {
	std::map<std::string, int> byNum;
	for (int i = 0; i < int(rn.size()); i++) if (!rn[i].empty()) byNum[rn[i]] = i;
	for (int i = 0; i < int(presses.size()); i++) {
		auto it = byNum.find(pn[i]);
		presses[i].partner = it == byNum.end() ? -1 : it->second;
	}
}

static fs::path resolvePackFolder() {
	auto custom = Mod::get()->getSettingValue<std::string>("pack-folder");
	std::error_code ec;
	if (!custom.empty()) return fs::path(custom);
	auto docs = documentsPack();
	if (!docs.empty() && fs::is_directory(docs / "player1" / "clicks", ec) && !fs::is_empty(docs / "player1" / "clicks", ec))
		return docs;
	return Mod::get()->getResourcesDir(); // the pack bundled with the mod
}

static int strength() {
	static const std::map<std::string, int> var = {{"Off", 0}, {"Natural (recommended)", 1}, {"More", 2}};
	auto v = var.find(Mod::get()->getSettingValue<std::string>("variation"));
	return v == var.end() ? 1 : v->second;
}

static void applySettings() {
	auto mod = Mod::get();
	static const std::map<std::string, float> spam = {
	    {"Same as normal clicks", 0.f}, {"A little quieter", -4.4f}, {"Like a real hand (recommended)", -8.4f}, {"Very quiet", -15.f}};
	auto s = spam.find(mod->getSettingValue<std::string>("spam"));
	g_engine.spamMinDB = s == spam.end() ? -8.4f : s->second;
	g_engine.gainVarDB = strength() == 0 ? 0.f : (strength() == 2 ? 3.5f : 2.5f);
	g_engine.playReleases = mod->getSettingValue<bool>("releases");
}

static void ensureLoaded() {
	auto mod = Mod::get();
	auto folder = resolvePackFolder();
	bool clean = mod->getSettingValue<bool>("clean-tone");
	int variations = int(std::clamp<int64_t>(mod->getSettingValue<int64_t>("variations"), 1, 200000));
	auto key = folder.string() + "|" + std::to_string(clean) + "|" + std::to_string(variations);
	if (key == g_loadedKey) { applySettings(); return; }
	g_engine = scs::Engine{};
	g_pack = scs::Pack{};
	g_engine.variations = variations;
	applySettings();
	std::vector<std::string> cn, rn, scn, srn;
	std::error_code ec;
	if (fs::is_directory(folder / "player1" / "clicks", ec)) { // clickpack layout
		auto p = folder / "player1";
		loadDir(p / "clicks", "", 0, cn, clean);
		loadDir(p / "releases", "", 1, rn, clean);
		loadDir(p / "softclicks", "", 2, scn, clean);
		loadDir(p / "softreleases", "", 3, srn, clean);
	} else if (fs::is_directory(folder / "clicks", ec)) { // plain clicks/releases folders
		loadDir(folder / "clicks", "", 0, cn, clean);
		loadDir(folder / "releases", "", 1, rn, clean);
	} else { // bundled pack: flat files
		loadDir(folder, "scs_click_", 0, cn, clean);
		loadDir(folder, "scs_release_", 1, rn, clean);
	}
	pairUp(g_engine.clicks, cn, rn);
	pairUp(g_engine.softClicks, scn, srn);
	g_engine.load();
	g_loadedKey = key;
	log::info("Smart Click Sounds: {} clicks, {} releases, {} light clicks from {} ({} variations each)",
	          g_engine.clicks.size(), g_engine.releases.size(), g_engine.softClicks.size(), folder.string(), variations);
}

// ---------- playback ----------

static void releaseFinished() {
	g_live.erase(std::remove_if(g_live.begin(), g_live.end(), [](Live& l) {
		bool playing = false;
		if (l.channel && l.channel->isPlaying(&playing) == FMOD_OK && playing) return false;
		l.sound->release();
		return true;
	}), g_live.end());
}

static void play(int player, int kind, scs::Play const& p) {
	scs::Buf pcm = g_pack.render(g_engine, p, strength());
	if (pcm.empty()) return;
	auto sys = FMODAudioEngine::sharedEngine()->m_system;
	if (!g_group) sys->createChannelGroup("SmartClickSounds", &g_group);
	releaseFinished();

	FMOD_CREATESOUNDEXINFO ex{};
	ex.cbsize = sizeof(ex);
	ex.length = unsigned(pcm.size() * sizeof(float));
	ex.numchannels = 1;
	ex.defaultfrequency = scs::kRate;
	ex.format = FMOD_SOUND_FORMAT_PCMFLOAT;
	FMOD::Sound* snd = nullptr;
	if (sys->createSound(reinterpret_cast<const char*>(pcm.data()), FMOD_OPENMEMORY | FMOD_OPENRAW | FMOD_CREATESAMPLE | FMOD_LOOP_OFF, &ex, &snd) != FMOD_OK || !snd)
		return;

	int rate = scs::kRate;
	sys->getSoftwareFormat(&rate, nullptr, nullptr);
	// fade out the previous sound of the same kind over 40 ms so tails don't pile up
	if (auto prev = g_last[player][kind]) {
		bool playing = false;
		unsigned long long parent = 0;
		if (prev->isPlaying(&playing) == FMOD_OK && playing && prev->getDSPClock(nullptr, &parent) == FMOD_OK) {
			unsigned long long fade = (unsigned long long)rate * 40 / 1000;
			if (p.delay > 0) parent += (unsigned long long)(p.delay * rate);
			prev->addFadePoint(parent, 1.f);
			prev->addFadePoint(parent + fade, 0.f);
			prev->setDelay(0, parent + fade, true);
		}
	}
	FMOD::Channel* ch = nullptr;
	if (sys->playSound(snd, g_group, true, &ch) != FMOD_OK || !ch) { snd->release(); return; }
	float vol = float(Mod::get()->getSettingValue<int64_t>("volume")) / 100.f;
	ch->setVolume(p.gain * vol);
	if (p.delay > 0) {
		unsigned long long parent = 0;
		if (ch->getDSPClock(nullptr, &parent) == FMOD_OK)
			ch->setDelay(parent + (unsigned long long)(p.delay * rate), 0, false);
	}
	ch->setPaused(false);
	g_last[player][kind] = ch;
	g_live.push_back({snd, ch});
}

// ---------- room tone (optional) ----------

static void stopRoomTone() {
	if (g_roomChannel) g_roomChannel->stop();
	g_roomChannel = nullptr;
}

static void startRoomTone() {
	stopRoomTone();
	auto mod = Mod::get();
	if (!mod->getSettingValue<bool>("enabled") || !mod->getSettingValue<bool>("room-tone")) return;
	auto sys = FMODAudioEngine::sharedEngine()->m_system;
	if (!g_group) sys->createChannelGroup("SmartClickSounds", &g_group);
	if (!g_roomSound) {
		scs::Buf x = scs::roomTone(6.f, -60.f);
		FMOD_CREATESOUNDEXINFO ex{};
		ex.cbsize = sizeof(ex);
		ex.length = unsigned(x.size() * sizeof(float));
		ex.numchannels = 1;
		ex.defaultfrequency = scs::kRate;
		ex.format = FMOD_SOUND_FORMAT_PCMFLOAT;
		if (sys->createSound(reinterpret_cast<const char*>(x.data()), FMOD_OPENMEMORY | FMOD_OPENRAW | FMOD_CREATESAMPLE | FMOD_LOOP_NORMAL, &ex, &g_roomSound) != FMOD_OK)
			g_roomSound = nullptr;
	}
	if (!g_roomSound) return;
	if (sys->playSound(g_roomSound, g_group, true, &g_roomChannel) != FMOD_OK || !g_roomChannel) { g_roomChannel = nullptr; return; }
	float vol = float(mod->getSettingValue<int64_t>("volume")) / 100.f;
	g_roomChannel->setVolume(vol);
	g_roomChannel->setPaused(false);
}

static void onInput(PlayerObject* obj, PlayerButton button, bool down) {
	auto mod = Mod::get();
	if (!mod->getSettingValue<bool>("enabled")) return;
	if (mod->getSettingValue<bool>("only-jump") && button != PlayerButton::Jump) return;
	auto gl = GJBaseGameLayer::get();
	if (!gl) return;
	GJGameLevel* level = nullptr;
	if (auto pl = PlayLayer::get()) {
		if (pl->m_isPaused) return;
		level = pl->m_level;
	} else if (auto el = LevelEditorLayer::get()) {
		level = el->m_level;
	} else {
		return;
	}
	int player;
	if (obj == gl->m_player1) player = 0;
	else if (obj == gl->m_player2 && level && level->m_twoPlayerMode) player = 1;
	else return; // in dual mode one click moves both players: play it once
	ensureLoaded();
	if (!g_engine.ready()) return;
	double t = gl->m_gameState.m_levelTime; // game time: unaffected by speedhack
	auto p = down ? g_engine.press(player, t) : g_engine.release(player, t);
	if (p) play(player, down ? 0 : 1, *p);
}

class $modify(SCSPlayer, PlayerObject) {
	bool pushButton(PlayerButton button) {
		bool ret = PlayerObject::pushButton(button);
		onInput(this, button, true);
		return ret;
	}
	bool releaseButton(PlayerButton button) {
		bool ret = PlayerObject::releaseButton(button);
		onInput(this, button, false);
		return ret;
	}
};

class $modify(SCSPlayLayer, PlayLayer) {
	bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
		if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
		ensureLoaded(); // load the pack before the first click, not during it
		g_engine.resetAll();
		startRoomTone();
		return true;
	}
	void onQuit() {
		stopRoomTone();
		PlayLayer::onQuit();
	}
	void resetLevel() {
		PlayLayer::resetLevel();
		g_engine.resetAll();
	}
};
