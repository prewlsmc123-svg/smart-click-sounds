// Offline preview: runs the mod's exact engine + sound code over macro inputs
// (CSV of time,down,player2) and writes the result as a WAV you can listen to.
//   render <pack dir with player1/clicks...> <inputs.csv> <out.wav> [strength 0-2] [spam dB]
#include "../src/voices.hpp"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
namespace fs = std::filesystem;
using namespace scs;

static Buf readWav(fs::path const& p) {
	std::ifstream f(p, std::ios::binary);
	std::vector<char> d((std::istreambuf_iterator<char>(f)), {});
	auto u16 = [&](size_t o) { return uint16_t(uint8_t(d[o]) | uint8_t(d[o + 1]) << 8); };
	auto u32 = [&](size_t o) { return uint32_t(u16(o) | uint32_t(u16(o + 2)) << 16); };
	size_t pos = 12, off = 0, len = 0; int ch = 1, bits = 16;
	while (pos + 8 <= d.size()) {
		std::string id(d.data() + pos, 4); size_t sz = u32(pos + 4), body = pos + 8;
		if (id == "fmt ") { ch = u16(body + 2); bits = u16(body + 14); }
		if (id == "data") { off = body; len = std::min(sz, d.size() - body); }
		pos = body + sz + (sz & 1);
	}
	Buf x;
	if (bits != 16) return x;
	for (size_t i = 0; i + 2 * ch <= len; i += 2 * ch) {
		float s = 0;
		for (int c = 0; c < ch; c++) s += int16_t(u16(off + i + 2 * c)) / 32768.f;
		x.push_back(s / ch);
	}
	return x;
}
static void writeWav(fs::path const& p, Buf const& x) {
	std::ofstream f(p, std::ios::binary);
	auto w32 = [&](uint32_t v) { f.write((char*)&v, 4); };
	auto w16 = [&](uint16_t v) { f.write((char*)&v, 2); };
	f.write("RIFF", 4); w32(36 + x.size() * 2); f.write("WAVEfmt ", 8); w32(16); w16(1); w16(1); w32(kRate); w32(kRate * 2); w16(2); w16(16);
	f.write("data", 4); w32(x.size() * 2);
	for (float v : x) { int s = int(std::lround(std::clamp(v, -1.f, 1.f) * 32767)); w16(uint16_t(int16_t(s))); }
}
static std::string num(std::string n) { n = n.substr(0, n.rfind('.')); size_t i = n.size(); while (i && isdigit(n[i - 1])) i--; return n.substr(i); }

static void loadList(fs::path dir, std::vector<Sound>& out, std::vector<Buf>& pcm, std::vector<std::string>& nums) {
	if (!fs::is_directory(dir)) return;
	std::vector<fs::path> files;
	for (auto& e : fs::directory_iterator(dir)) if (e.path().extension() == ".wav") files.push_back(e.path());
	std::sort(files.begin(), files.end());
	for (auto& f : files) {
		Buf x = readWav(f);
		if (x.empty()) continue;
		cleanTone(x);
		out.push_back({nullptr, loudnessDB(x.data(), x.size(), kRate), -1});
		pcm.push_back(std::move(x));
		nums.push_back(num(f.filename().string()));
	}
}
static void pair(std::vector<Sound>& c, std::vector<std::string> const& cn, std::vector<std::string> const& rn) {
	std::map<std::string, int> m; for (int i = 0; i < int(rn.size()); i++) m[rn[i]] = i;
	for (int i = 0; i < int(c.size()); i++) { auto it = m.find(cn[i]); c[i].partner = it == m.end() ? -1 : it->second; }
}

int main(int argc, char** argv) {
	if (argc < 4) { std::puts("usage: render pack inputs.csv out.wav [strength] [spamDB]"); return 1; }
	Engine e; Pack pk;
	int strength = argc > 4 ? atoi(argv[4]) : 1;
	if (argc > 5) e.spamMinDB = float(atof(argv[5]));
	fs::path p = fs::path(argv[1]) / "player1";
	std::vector<std::string> cn, rn, scn, srn;
	loadList(p / "clicks", e.clicks, pk.pcm[0], cn);
	loadList(p / "releases", e.releases, pk.pcm[1], rn);
	loadList(p / "softclicks", e.softClicks, pk.pcm[2], scn);
	loadList(p / "softreleases", e.softReleases, pk.pcm[3], srn);
	pair(e.clicks, cn, rn); pair(e.softClicks, scn, srn);
	e.load();
	std::vector<std::tuple<double, bool, int>> acts;
	std::ifstream f(argv[2]); std::string line;
	while (std::getline(f, line)) { double t; int d, q; if (sscanf(line.c_str(), "%lf,%d,%d", &t, &d, &q) == 3) acts.push_back({t, d == 1, q}); }
	struct V { Buf s; long start; long stop = -1; float g; };
	std::vector<V> voices; long last[2][2] = {{-1, -1}, {-1, -1}};
	double end = 0; int presses = 0;
	std::map<int, int> varUse;
	for (auto& [t, down, q] : acts) {
		end = std::max(end, t);
		auto pl = down ? e.press(q, t) : e.release(q, t);
		if (!pl) continue;
		if (down) { presses++; varUse[pl->variation]++; }
		long start = long((t + pl->delay) * kRate);
		int kind = down ? 0 : 1;
		if (kind == 0 && last[q][1] >= 0 && voices[last[q][1]].start > start) voices[last[q][1]].s.clear(); // release not started yet
		if (last[q][kind] >= 0) voices[last[q][kind]].stop = start;
		voices.push_back({pk.render(e, *pl, strength), start, -1, pl->gain});
		last[q][kind] = long(voices.size()) - 1;
	}
	Buf out(size_t((end + 1.5) * kRate));
	const long fade = kRate * 40 / 1000;
	for (auto& v : voices)
		for (long i = 0; i < long(v.s.size()); i++) {
			long j = v.start + i; if (j >= long(out.size())) break;
			float g = v.g;
			if (v.stop >= 0 && j >= v.stop) { long k = j - v.stop; if (k >= fade) break; g *= float(0.5 * (1 + std::cos(kPi * k / fade))); }
			out[j] += v.s[i] * g;
		}
	// same level relation as in game: clicks at their natural level, room tone at -60 dB RMS
	float pk2 = 0; for (float v : out) pk2 = std::max(pk2, std::fabs(v));
	float norm = pk2 > 0 ? 0.891f / pk2 : 1.f;
	for (auto& v : out) v *= norm;
	if (argc > 6 && std::string(argv[6]) == "room") {
		Buf room = roomTone(6.f, -60.f);
		for (size_t i = 0; i < out.size(); i++) out[i] += room[i % room.size()] * norm;
	}
	writeWav(argv[3], out);
	int reused = 0; for (auto& [k, n] : varUse) if (n > 1) reused++;
	std::printf("presses %d, different variations %zu (reused %d), length %.1fs\n", presses, varUse.size(), reused, out.size() / double(kRate));
}
