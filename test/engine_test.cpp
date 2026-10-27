#include "../src/voices.hpp"
#include <cstdio>
#include <deque>
#include <fstream>
#include <map>
#include <string>
using namespace scs;
int fails = 0;
float g_maxJump = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); fails++; } } while (0)

int main(int argc, char** argv) {
	Engine e;
	std::mt19937 r(3);
	for (int i = 0; i < 170; i++) {
		float l = -30 + 25 * std::uniform_real_distribution<float>(0, 1)(r);
		e.clicks.push_back({nullptr, l, i < 165 ? i : -1});
		if (i < 165) e.releases.push_back({nullptr, l - 12, -1});
	}
	e.load();
	std::vector<std::tuple<double, bool, int>> acts;
	std::ifstream f(argv[1]); std::string line;
	while (std::getline(f, line)) { double t; int d, p; if (sscanf(line.c_str(), "%lf,%d,%d", &t, &d, &p) == 3) acts.push_back({t, d == 1, p}); }
	int downs = 0, presses = 0, own = 0, rels = 0;
	std::map<int, int> lastVar; std::deque<int> lastBases[2];
	int lastPressBase[2] = {-1, -1}; int k = 0; int ownable = 0; double lastT[2] = {-10, -10};
	std::vector<float> lv[2]; float refL = 0;
	{ std::vector<float> L; for (auto& c : e.clicks) L.push_back(c.loud); std::sort(L.begin(), L.end()); refL = L[L.size() * 7 / 10]; }
	for (auto& [t, down, p2] : acts) {
		if (down) {
			downs++;
			auto p = e.press(p2, t); if (!p) continue;
			presses++; k++;
			auto it = lastVar.find(p->variation);
			CHECK(it == lastVar.end() || k - it->second >= 32, "variation reused after %d", k - it->second);
			lastVar[p->variation] = k;
			for (int b : lastBases[0]) if (b == p->base) { std::printf("  repeat base %d at t=%.3f (variation %d)\n", p->base, t, p->variation); fails++; }
			lastBases[0].push_back(p->base); if (lastBases[0].size() > 8) lastBases[0].pop_front();
			lastPressBase[p2] = p->base;
			float lvl = e.clicks[p->base].loud + 20 * std::log10(p->gain) - refL;
			if (t - lastT[p2] < 0.8 && !lv[p2].empty()) { g_maxJump = std::max(g_maxJump, lvl - lv[p2].back()); lv[p2].push_back(lvl); } else { lv[p2].clear(); lv[p2].push_back(lvl); }
			lastT[p2] = t;
		} else {
			auto p = e.release(p2, t); if (!p) continue;
			rels++;
			if (lastPressBase[p2] >= 0 && e.clicks[lastPressBase[p2]].partner >= 0) ownable++;
			if (lastPressBase[p2] >= 0 && e.clicks[lastPressBase[p2]].partner == p->base && p->variation == p->pressVariation) own++;
			CHECK(p->delay >= 0 && p->delay <= 0.066, "bad delay");
		}
	}
	float maxJump = g_maxJump;
	std::printf("downs %d presses %d releases %d (own %d) max jump %.1f dB\n", downs, presses, rels, own, maxJump);
	CHECK(presses >= downs * 95 / 100, "too few presses");
	CHECK(own == ownable, "releases not matched (%d of %d)", own, ownable);
	CHECK(maxJump < 10, "jump too big");
	// up-time rule: spam (finger up 40 ms) much lighter than relaxed (300 ms up)
	CHECK(e.upTimeDB(0.3) == 0 && e.upTimeDB(0.04) < -8 && e.upTimeDB(0.02) < e.upTimeDB(0.04), "up-time curve");
	CHECK(Engine::holdDB(0.03) == 5 && Engine::holdDB(0.065) == 0 && Engine::holdDB(0.3) == -7, "hold curve");
	// a variation always renders the same and differs from its original
	Pack pk; for (int i = 0; i < 170; i++) { Buf b(4800); for (int j = 0; j < 4800; j++) b[j] = float(std::sin(j * (0.1 + i * 0.001)) * std::exp(-j / 800.0)); pk.pcm[0].push_back(b); }
	for (int i = 0; i < 165; i++) pk.pcm[1].push_back(pk.pcm[0][i]);
	Play a; a.list = 0; a.base = 5; a.variation = 5 + 170 * 7;
	Buf x1 = pk.render(e, a, 1), x2 = pk.render(e, a, 1);
	Play o = a; o.variation = 5; Buf x0 = pk.render(e, o, 1);
	CHECK(x1 == x2, "variation not deterministic");
	CHECK(x1 != x0, "variation identical to original");
	std::printf(fails ? "%d FAILURES\n" : "ALL OK\n", fails);
	return fails != 0;
}
