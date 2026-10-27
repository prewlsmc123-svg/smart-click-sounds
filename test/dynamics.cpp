// checks the engine's loudness behaviour against measurements of real clicking
#include "../src/engine.hpp"
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>
using namespace scs;
static double corr(std::vector<double> const& a, std::vector<double> const& b) {
	double ma = 0, mb = 0; for (size_t i = 0; i < a.size(); i++) { ma += a[i]; mb += b[i]; } ma /= a.size(); mb /= b.size();
	double sab = 0, saa = 0, sbb = 0; for (size_t i = 0; i < a.size(); i++) { sab += (a[i]-ma)*(b[i]-mb); saa += (a[i]-ma)*(a[i]-ma); sbb += (b[i]-mb)*(b[i]-mb); }
	return sab / std::sqrt(saa * sbb);
}
int main(int, char** argv) {
	Engine e; std::mt19937 r(3);
	for (int i = 0; i < 170; i++) { float l = -18 + 4 * std::uniform_real_distribution<float>(0,1)(r); e.clicks.push_back({nullptr, l, i}); e.releases.push_back({nullptr, l - 12, -1}); }
	e.load();
	std::vector<double> press, rel, pressRelaxed; double lastRel = -10;
	// a relaxed human-ish session: presses every 0.3-0.7 s, holds 50-150 ms
	double t = 1;
	for (int i = 0; i < 4000; i++) {
		auto p = e.press(0, t); double hold = 0.05 + std::uniform_real_distribution<double>(0, 0.1)(r);
		auto q = e.release(0, t + hold);
		if (p && q) { press.push_back(20 * std::log10(p->gain) + e.clicks[p->base].loud); rel.push_back(20 * std::log10(q->gain) + e.clicks[p->base].loud); }
		t += hold + 0.3 + std::uniform_real_distribution<double>(0, 0.4)(r);
	}
	std::vector<double> a(press.begin() + 1, press.end()), b(press.begin(), press.end() - 1);
	double m = 0, sd = 0; for (double v : press) m += v; m /= press.size(); for (double v : press) sd += (v - m) * (v - m); sd = std::sqrt(sd / press.size());
	std::printf("press loudness spread %.1f dB (real ~2.5 robust) | press vs previous corr %.2f (real 0.69) | press vs own release corr %.2f (real 0.37)\n",
	            sd, corr(a, b), corr(press, rel));
}
