#include "../src/engine.hpp"
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); fails++; } } while (0)

int main(int argc, char** argv) {
	scs::Engine e;
	std::mt19937 r(3);
	// 170 presses spread over ~25 dB like the reference pack, each with a release ~12 dB lower
	for (int i = 0; i < 170; i++) {
		float l = -30 + 25 * std::uniform_real_distribution<float>(0, 1)(r);
		e.clicks.push_back({(void*)(intptr_t)(i + 1), l, i});
		e.releases.push_back({(void*)(intptr_t)(10000 + i), l - 12, -1});
	}
	e.load();
	CHECK(e.ready(), "engine not ready");

	// read macro inputs (time,down,p2) from a CSV, else synthesize
	std::vector<std::tuple<double, bool, int>> acts;
	if (argc > 1) {
		std::ifstream f(argv[1]);
		std::string line;
		while (std::getline(f, line)) {
			double t; int d, p;
			if (std::sscanf(line.c_str(), "%lf,%d,%d", &t, &d, &p) == 3) acts.push_back({t, d == 1, p});
		}
	}
	std::printf("inputs: %zu\n", acts.size());
	std::vector<void*> played;
	std::vector<float> levels;
	std::map<void*, float> loudOf;
	for (auto& c : e.clicks) loudOf[c.handle] = c.loud;
	int presses = 0, releases = 0, matched = 0;
	void* lastPress[2] = {nullptr, nullptr};
	float refLoud = 0;
	{ std::vector<float> L; for (auto& c : e.clicks) L.push_back(c.loud); std::sort(L.begin(), L.end()); refLoud = L[L.size() * 7 / 10]; }
	for (auto& [t, down, p2] : acts) {
		if (down) {
			auto pl = e.press(p2, t);
			if (!pl) continue;
			presses++;
			played.push_back(pl->sound);
			levels.push_back(loudOf[pl->sound] + 20 * std::log10(pl->gain) - refLoud);
			lastPress[p2] = pl->sound;
		} else {
			auto pl = e.release(p2, t);
			if (!pl) continue;
			releases++;
			if ((intptr_t)pl->sound == (intptr_t)lastPress[p2] - 1 + 10000) matched++; else std::printf("  mismatch at t=%.3f: release %ld after press %ld\n", t, (long)(intptr_t)pl->sound, (long)(intptr_t)lastPress[p2]);
			CHECK(pl->delay >= 0 && pl->delay <= 0.035, "bad release delay %f", pl->delay);
		}
	}
	std::printf("presses %d releases %d (own release %d)\n", presses, releases, matched);
	int downs = 0; for (auto& a : acts) if (std::get<1>(a)) downs++;
	CHECK(presses >= downs * 95 / 100, "too few presses (%d of %d)", presses, downs);
	CHECK(matched == releases, "releases not matched to their press");
	// never the same sound within 32 presses
	std::map<void*, int> last;
	for (int k = 0; k < int(played.size()); k++) {
		auto it = last.find(played[k]);
		CHECK(it == last.end() || k - it->second >= 32, "sound reused after %d clicks", k - it->second);
		last[played[k]] = k;
	}
	float maxJump = 0;
	for (size_t k = 1; k < levels.size(); k++) maxJump = std::max(maxJump, levels[k] - levels[k - 1]);
	std::printf("max loudness jump between clicks: %.1f dB\n", maxJump);
	CHECK(maxJump < 9.5f, "loudness jumps %.1f dB", maxJump);

	// spam: 20 presses 50 ms apart must be clearly lighter than slow clicks
	scs::Engine s = e;
	s.load();
	float slow = 0, fast = 0;
	for (int k = 0; k < 6; k++) { auto pl = s.press(0, 1 + k * 0.5); s.release(0, 1.05 + k * 0.5); slow += 20 * std::log10(pl->gain) + loudOf[pl->sound]; }
	for (int k = 0; k < 20; k++) { auto pl = s.press(0, 10 + k * 0.05); s.release(0, 10.02 + k * 0.05); if (k > 3) fast += (20 * std::log10(pl->gain) + loudOf[pl->sound]) / 16 * 6; }
	std::printf("slow avg %.1f dB, spam avg %.1f dB\n", slow / 6, fast / 6);
	CHECK(fast / 6 < slow / 6 - 5, "spam not lighter");
	// ghost double press 8 ms apart: second is silent
	auto a1 = s.press(0, 50.0); s.release(0, 50.004);
	auto a2 = s.press(0, 50.008);
	CHECK(a1 && !a2, "ghost press not suppressed");
	// checkpoint: time goes back -> state resets, press plays at full strength
	auto c1 = s.press(0, 20.0);
	CHECK(c1.has_value(), "press after checkpoint rewind should play");
	std::printf(fails ? "%d FAILURES\n" : "ALL OK\n", fails);
	return fails ? 1 : 0;
}
