#pragma once
// Smart Click Sounds engine: decides which click variation to play for each
// press and release, and how loud, from game-time input timing. Plain C++.
//
// Rules (from ZCB3's click-type logic and measurements of real clicking):
//  * A press's strength depends on how long the finger was UP (time since the
//    last release): 150 ms+ = full; spam (< 150 ms) gets lighter down to the
//    "spam" setting at ~40 ms; "micro" re-presses (< 25 ms up) are lighter still.
//  * A release depends on how long it was HELD: quick taps let go snappier
//    (+2 dB), long holds softer (-2 dB). It always plays the release recorded
//    with that press's click, varied the same way.
//  * Loudness never jumps up suddenly after quiet clicks; it climbs back.
//  * Picks among N variations (default 10,000) of the recorded clicks, the one
//    whose loudness fits; every variation is used once before any repeats, and
//    the same recorded click is never the source twice within 8 clicks.
//  * A second press < 25 ms after the previous press (input buffering) is silent.
//  * Releases can't come < 35 ms after their press (a finger can't): delayed.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <optional>
#include <random>
#include <vector>

namespace scs {

struct Sound {
	void* handle = nullptr; // optional user data (e.g. decoded audio)
	float loud = -100.f;    // dB, loudest 10 ms of the first 40 ms
	int partner = -1;       // index of its release in the matching release list
};

// loudness of mono float samples (same measure as the GDClickbot app)
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

// A bank of N variations of nBase recorded sounds (variation k comes from
// recording k % nBase), sorted by loudness.
class Bank {
public:
	int nBase = 0;
	std::vector<int> ids;      // variation ids sorted by loudness
	std::vector<float> loud;   // ascending
	std::vector<bool> used;
	std::deque<int> recentIds, recentBase;
	int nUsed = 0;
	float ref = -20.f;         // a typical firm click (70th percentile)

	void build(std::vector<Sound> const& base, int variations) {
		nBase = int(base.size());
		ids.clear(); loud.clear(); recentIds.clear(); recentBase.clear(); nUsed = 0;
		if (!nBase) return;
		int n = std::max(variations, nBase);
		for (int k = 0; k < n; k++) ids.push_back(k);
		std::stable_sort(ids.begin(), ids.end(), [&](int a, int b) { return base[a % nBase].loud < base[b % nBase].loud; });
		for (int k : ids) loud.push_back(base[k % nBase].loud);
		used.assign(n, false);
		std::vector<float> L;
		for (auto& s : base) L.push_back(s.loud);
		std::sort(L.begin(), L.end());
		ref = L[L.size() * 7 / 10];
	}
	bool empty() const { return ids.empty(); }

	// returns a variation id whose loudness is close to target
	int pick(float target, std::mt19937& rng, float& outLoud) {
		int n = int(ids.size());
		if (nUsed >= n) { // all used: start over (recent ones stay off-limits below)
			std::fill(used.begin(), used.end(), false);
			nUsed = 0;
		}
		auto baseRecent = [&](int id) {
			return std::find(recentBase.begin(), recentBase.end(), id % nBase) != recentBase.end();
		};
		auto idRecent = [&](int id) { return std::find(recentIds.begin(), recentIds.end(), id) != recentIds.end(); };
		int pos = int(std::lower_bound(loud.begin(), loud.end(), target) - loud.begin());
		std::vector<int> cand, candBases;
		for (int pass = 0; pass < 2 && cand.empty(); pass++) {
			int lo = pos - 1, hi = pos;
			while (cand.size() < 4 && (lo >= 0 || hi < n)) {
				bool useHi = hi < n && (lo < 0 || loud[hi] - target <= target - loud[lo]);
				int j = useHi ? hi++ : lo--;
				if (std::fabs(loud[j] - target) > 8 && !cand.empty()) break;
				int id = ids[j];
				if (idRecent(id)) continue;
				if (pass == 0 && (used[j] || baseRecent(id))) continue;
				if (std::find(candBases.begin(), candBases.end(), id % nBase) != candBases.end()) continue;
				cand.push_back(j);
				candBases.push_back(id % nBase);
			}
		}
		if (cand.empty()) cand.push_back(std::min(std::max(pos, 0), n - 1));
		int j = cand[std::uniform_int_distribution<int>(0, int(cand.size()) - 1)(rng)];
		if (!used[j]) { used[j] = true; nUsed++; }
		int id = ids[j];
		recentIds.push_back(id);
		if (recentIds.size() > 32) recentIds.pop_front();
		recentBase.push_back(id % nBase);
		if (recentBase.size() > size_t(std::min(8, std::max(1, nBase / 3)))) recentBase.pop_front();
		outLoud = loud[j];
		return id;
	}
};

struct Play {
	int list = 0;         // 0 clicks, 1 releases, 2 softclicks, 3 softreleases
	int base = -1;        // recorded sound index in that list
	int variation = 0;    // variation id (variation % listSize == base for presses)
	int pressList = 0;    // for releases: the list/variation of its press (to vary alike)
	int pressVariation = 0;
	float gain = 1;       // linear volume multiplier
	float tiltDB = 0;     // extra brightness: harder presses are brighter
	double delay = 0;     // seconds to wait before playing
	bool cutPrevious = true;
};

class Engine {
public:
	std::vector<Sound> clicks, releases, softClicks, softReleases;
	float spamMinDB = -8.4f;  // "Like a real hand"
	float gainVarDB = 2.5f;   // natural loudness spread (std, dB), measured on real clicking
	int variations = 10000;
	bool playReleases = true;

	void load() {
		cb.build(clicks, variations);
		rb.build(releases, variations);
		scb.build(softClicks, variations);
		srb.build(softReleases, variations);
		resetAll();
	}
	bool ready() const { return !cb.empty(); }
	void resetAll() { for (auto& p : players) p = PlayerState{}; }

	// press loudness (dB) from how long the finger was up
	float upTimeDB(double up) const {
		if (up >= 0.16) return 0;
		double f = up > 0.035 ? std::log(up / 0.035) / std::log(0.16 / 0.035) : 0;
		f = f * f * (3 - 2 * f);
		float db = float(spamMinDB * (1 - f));
		if (up < 0.025) db += spamMinDB < 0 ? -3.f : 0.f; // micro
		return db;
	}
	// release loudness change from hold time (measured on two real sessions,
	// mouse and keyboard, which agree): quick taps let go hard, long holds softly
	static float holdDB(double hold) {
		if (hold <= 0.04) return 5.f;
		if (hold <= 0.065) return float(5 * (0.065 - hold) / 0.025);
		if (hold <= 0.14) return float(-7 * (hold - 0.065) / 0.075);
		return -7.f;
	}

	std::optional<Play> press(int player, double t) {
		if (cb.empty()) return std::nullopt;
		PlayerState& s = players[player & 1];
		if (t < s.lastPress - 0.001) s = PlayerState{}; // time went back: respawn/checkpoint
		if (s.down) return std::nullopt;
		s.down = true;
		double dt = t - s.lastPress;
		s.ghost = dt < 0.025;
		if (s.ghost) return std::nullopt;
		double up = t - s.lastRelease;
		float rel = upTimeDB(up);
		// Human dynamics: loudness drifts in waves (a real press is ~0.7 correlated
		// with the previous one) instead of being random per click.
		if (gainVarDB > 0) {
			float rho = dt < 1.0 ? 0.7f : 0.35f;
			std::normal_distribution<float> nd(0, gainVarDB);
			s.drift = rho * s.drift + std::sqrt(1 - rho * rho) * nd(rng);
			s.drift = std::clamp(s.drift, -2 * gainVarDB, 2 * gainVarDB);
			rel += s.drift;
		} else {
			s.drift = 0;
		}
		if (dt < 0.8) { // climb back gradually after quiet clicks
			float maxRise = float(2 + 8 * dt);
			rel = std::min(rel, s.rel + maxRise);
			rel = std::max(rel, s.rel - 8);
		}
		s.rel = rel;
		s.prevDt = dt;
		s.lastPress = s.pressAt = t;
		bool soft = up < 0.15 && !scb.empty();
		Bank& b = soft ? scb : cb;
		float target = cb.ref + rel, l = 0;
		int id = b.pick(target, rng, l);
		float gain = std::clamp(std::pow(10.f, (target - l) / 20.f), 0.03f, 2.0f);
		s.list = soft ? 2 : 0;
		s.variation = id;
		s.gain = gain;
		s.tilt = 0.3f * s.drift; // harder presses ring brighter (measured ~0.4 correlation)
		Play p;
		p.list = s.list;
		p.base = id % b.nBase;
		p.variation = id;
		p.gain = gain;
		p.tiltDB = s.tilt;
		return p;
	}

	std::optional<Play> release(int player, double t) {
		PlayerState& s = players[player & 1];
		if (!s.down) return std::nullopt;
		s.down = false;
		s.lastRelease = t;
		if (s.ghost || !playReleases) return std::nullopt;
		Play p;
		// a bot's 1-4 tick tap (< 20 ms) is faster than any finger, so its release
		// is moved to a fast-but-human 38-50 ms after the press (cancelled if the
		// next press comes first). Holds of 20 ms+ keep their real timing.
		double hold = t - s.pressAt;
		if (hold < 0.02) {
			double want = std::uniform_real_distribution<double>(0.038, 0.05)(rng);
			// in fast tapping the next press may come before that: keep the moved
			// release well clear of it (by the current rhythm), or it lands right
			// before the next click and the two sound like one double click
			if (s.prevDt < 0.4) want = std::min(want, std::max(hold, std::min(0.55 * s.prevDt, s.prevDt - 0.035)));
			p.delay = std::max(0.0, s.pressAt + want - t);
			hold = std::max(hold, want);
		}
		p.pressList = s.list;
		p.pressVariation = s.variation;
		// a release only loosely follows its press's strength (real: ~0.4 correlation)
		float relDB = holdDB(hold) - 0.6f * s.drift;
		if (gainVarDB > 0) relDB += std::normal_distribution<float>(0, gainVarDB * 1.1f)(rng);
		float adj = std::pow(10.f, std::clamp(relDB, -12.f, 8.f) / 20.f);
		auto const& presses = s.list == 2 ? softClicks : clicks;
		int pb = s.variation % int(presses.size());
		if (presses[pb].partner >= 0) { // the release recorded with this click
			p.list = s.list == 2 ? 3 : 1;
			p.base = presses[pb].partner;
			p.variation = s.variation;
			p.gain = s.gain * adj;
			p.tiltDB = 0.5f * s.tilt;
			return p;
		}
		bool soft = s.list == 2 && !srb.empty();
		Bank& b = soft ? srb : rb;
		if (b.empty()) return std::nullopt;
		float target = rb.ref + s.rel, l = 0;
		int id = b.pick(target, rng, l);
		p.list = soft ? 3 : 1;
		p.base = id % b.nBase;
		p.variation = id;
		p.gain = std::clamp(std::pow(10.f, (target - l) / 20.f), 0.03f, 2.0f) * adj;
		return p;
	}

private:
	struct PlayerState {
		bool down = false, ghost = false;
		double lastPress = -10, lastRelease = -10, pressAt = -10, prevDt = 10;
		float rel = 0, gain = 1, drift = 0, tilt = 0;
		int list = 0, variation = 0;
	};
	PlayerState players[2];
	Bank cb, rb, scb, srb;
	std::mt19937 rng{std::random_device{}()};
};

} // namespace scs
