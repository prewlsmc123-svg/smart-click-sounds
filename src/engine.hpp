#pragma once
// Smart Click Sounds engine: decides which sound to play for each press and
// release, and how loud, from game-time input timing. Plain C++ (no Geode), so
// it can be unit tested on its own.
//
// What it does (same rules as the GDClickbot app):
//  * Each press gets a target loudness: full strength for normal clicks, lighter
//    the faster you spam (a curve matched to a real human spam recording).
//  * Loudness never jumps up suddenly after quiet clicks; it climbs back gradually.
//  * The pack sound whose natural loudness is closest is used (light taps are
//    real light taps, not loud clicks turned down), and every sound is used once
//    before any repeats; never one of the last 32 again.
//  * Real fast/spam clicks ("softclicks") are used for spam when the pack has them.
//  * Each press plays with its own release (recorded together), same pitch/volume.
//  * A release can't physically come < 35 ms after its press: it's delayed.
//  * A second press < 25 ms after the previous one (input buffering) is silent.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <random>
#include <vector>

namespace scs {

struct Sound {
	void* handle = nullptr; // FMOD::Sound* in the mod
	float loud = -100.f;    // dB, loudest 10 ms of the first 40 ms
	int partner = -1;       // index of its release in the matching release list
};

// loudness of mono float samples (same measure as the app)
inline float loudnessDB(const float* x, size_t n, int rate) {
	size_t w = rate / 100, len = std::min(n, size_t(rate) * 4 / 100);
	if (len == 0) return -100.f;
	if (w > len) w = len;
	double best = 0;
	size_t step = std::max<size_t>(1, w / 4);
	for (size_t s = 0; s + w <= len; s += step) {
		double sum = 0;
		for (size_t i = s; i < s + w; i++) sum += double(x[i]) * x[i];
		best = std::max(best, sum);
	}
	double r = std::sqrt(best / double(w));
	return r < 1e-9 ? -100.f : float(20 * std::log10(r));
}

class Bank {
public:
	std::vector<int> idx;      // sound indices sorted by loudness
	std::vector<float> loud;   // ascending
	std::vector<bool> used;
	std::vector<int> recent;
	int nUsed = 0;
	float ref = -20.f;         // a typical firm click (70th percentile)

	void build(const std::vector<Sound>& sounds) {
		idx.clear(); loud.clear(); recent.clear(); nUsed = 0;
		for (int i = 0; i < int(sounds.size()); i++) idx.push_back(i);
		std::sort(idx.begin(), idx.end(), [&](int a, int b) { return sounds[a].loud < sounds[b].loud; });
		for (int i : idx) loud.push_back(sounds[i].loud);
		used.assign(idx.size(), false);
		if (!loud.empty()) ref = loud[loud.size() * 7 / 10];
	}
	bool empty() const { return idx.empty(); }

	// returns the sound index (into the original list) and its loudness
	int pick(float target, std::mt19937& rng, float& outLoud) {
		int n = int(idx.size());
		size_t keep = std::min<size_t>(32, n / 2);
		if (nUsed >= n) { // pack used up: start over, keeping the most recent off-limits
			std::fill(used.begin(), used.end(), false);
			nUsed = 0;
			for (int r : recent) if (!used[r]) { used[r] = true; nUsed++; }
			if (nUsed >= n) { std::fill(used.begin(), used.end(), false); nUsed = 0; }
		}
		int pos = int(std::lower_bound(loud.begin(), loud.end(), target) - loud.begin());
		std::vector<int> cand;
		int lo = pos - 1, hi = pos;
		while (cand.size() < 4 && (lo >= 0 || hi < n)) {
			bool useHi = hi < n && (lo < 0 || loud[hi] - target <= target - loud[lo]);
			if (useHi) { if (!used[hi]) cand.push_back(hi); hi++; }
			else { if (!used[lo]) cand.push_back(lo); lo--; }
		}
		int k = cand[std::uniform_int_distribution<int>(0, int(cand.size()) - 1)(rng)];
		// unused sounds near this loudness all gone: reuse a close one that
		// hasn't played recently rather than a far-off (wrong-sounding) one
		if (std::fabs(loud[k] - target) > 8) {
			int best = -1;
			float bd = std::fabs(loud[k] - target) - 2;
			for (int j = 0; j < n; j++) {
				if (std::find(recent.begin(), recent.end(), j) != recent.end()) continue;
				float d = std::fabs(loud[j] - target);
				if (d < bd) { best = j; bd = d; }
			}
			if (best >= 0) {
				remember(best, keep);
				outLoud = loud[best];
				return idx[best];
			}
		}
		used[k] = true;
		nUsed++;
		remember(k, keep);
		outLoud = loud[k];
		return idx[k];
	}

private:
	void remember(int k, size_t keep) {
		recent.push_back(k);
		if (recent.size() > keep) recent.erase(recent.begin(), recent.end() - keep);
	}
};

// how loud a press is (0..1 gain) from the time since the previous press
inline float spamGain(double dt, int streak, float minGain) {
	if (minGain >= 1) return 1;
	const double slow = 0.15, fast = 0.06;
	if (dt >= slow) return 1;
	double f = dt > fast ? std::log(dt / fast) / std::log(slow / fast) : 0;
	f = f * f * (3 - 2 * f);
	double g = minGain + (1 - minGain) * f;
	if (streak > 4) g *= std::max(0.85, 1 - 0.03 * (streak - 4));
	return float(g);
}

struct Variation { float pitch, gainDB; };
inline const Variation kVariations[3] = {{0.f, 0.f}, {0.012f, 1.0f}, {0.025f, 2.0f}};

struct Play {
	void* sound = nullptr; // sound to play
	float gain = 1;        // linear volume multiplier
	float pitch = 1;       // playback speed/pitch multiplier
	double delay = 0;      // seconds to wait before playing (late releases)
	bool cutPrevious = false;
};

class Engine {
public:
	std::vector<Sound> clicks, releases, softClicks, softReleases;
	float spamMinGain = 0.38f; // "Like your reference"
	int variation = 1;
	bool playReleases = true;

	void load() {
		cb.build(clicks); rb.build(releases); scb.build(softClicks); srb.build(softReleases);
		resetAll();
	}
	bool ready() const { return !cb.empty(); }
	void resetAll() { for (auto& p : players) p = PlayerState{}; }

	std::optional<Play> press(int player, double t) {
		if (cb.empty()) return std::nullopt;
		PlayerState& s = players[player & 1];
		if (t < s.lastPress - 0.001) s = PlayerState{}; // time went back: respawn/checkpoint
		if (s.down) return std::nullopt;
		s.down = true;
		double dt = t - s.lastPress;
		s.ghost = dt < 0.025;
		if (s.ghost) return std::nullopt;
		s.streak = dt < 0.22 ? s.streak + 1 : 0;
		float g = spamGain(dt, s.streak, spamMinGain);
		float rel = 20 * std::log10(g);
		const Variation& v = kVariations[std::clamp(variation, 0, 2)];
		if (v.gainDB > 0) {
			std::normal_distribution<float> nd(0, v.gainDB / 2);
			rel += std::clamp(nd(rng), -v.gainDB, v.gainDB);
		}
		if (dt < 0.8) { // climb back gradually after quiet clicks
			float maxRise = float(2 + 8 * dt);
			rel = std::min(rel, s.rel + maxRise);
			rel = std::max(rel, s.rel - 8);
		}
		s.rel = rel;
		s.lastPress = t;
		s.soft = g < 0.7f;
		bool useSoft = s.soft && !scb.empty();
		Bank& b = useSoft ? scb : cb;
		auto& list = useSoft ? softClicks : clicks;
		float target = cb.ref + rel, l = 0;
		int i = b.pick(target, rng, l);
		float gain = std::clamp(std::pow(10.f, (target - l) / 20.f), 0.03f, 2.0f);
		s.pitch = 1 + std::uniform_real_distribution<float>(-1, 1)(rng) * v.pitch;
		s.gain = gain;
		s.partner = list[i].partner >= 0 ? &(useSoft ? softReleases : releases)[list[i].partner] : nullptr;
		Play p;
		p.sound = list[i].handle;
		p.gain = gain;
		p.pitch = s.pitch;
		p.cutPrevious = true;
		return p;
	}

	std::optional<Play> release(int player, double t) {
		PlayerState& s = players[player & 1];
		if (!s.down) return std::nullopt;
		s.down = false;
		if (s.ghost || !playReleases) return std::nullopt;
		Play p;
		p.delay = std::max(0.0, s.lastPress + 0.035 - t); // a finger needs ~35 ms to let go
		p.pitch = s.pitch;
		p.cutPrevious = true;
		if (s.partner) { // the release recorded with this press
			p.sound = s.partner->handle;
			p.gain = s.gain;
			return p;
		}
		Bank& b = (s.soft && !srb.empty()) ? srb : rb;
		auto& list = (s.soft && !srb.empty()) ? softReleases : releases;
		if (b.empty()) return std::nullopt;
		float target = rb.empty() ? b.ref + s.rel : rb.ref + s.rel, l = 0;
		int i = b.pick(target, rng, l);
		p.sound = list[i].handle;
		p.gain = std::clamp(std::pow(10.f, (target - l) / 20.f), 0.03f, 2.0f);
		return p;
	}

private:
	struct PlayerState {
		bool down = false, ghost = false, soft = false;
		double lastPress = -10;
		float rel = 0, gain = 1, pitch = 1;
		int streak = 0;
		Sound* partner = nullptr;
	};
	PlayerState players[2];
	Bank cb, rb, scb, srb;
	std::mt19937 rng{std::random_device{}()};
};

} // namespace scs
