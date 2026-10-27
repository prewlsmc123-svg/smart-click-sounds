#pragma once
// Sound shaping for Smart Click Sounds (plain C++, testable outside GD).
//
//  * cleanTone(): removes desk "boom" (the clunky thunk) and keeps the crisp
//    click: high-pass at 120 Hz, a little presence at 4 kHz, and a low-shelf cut
//    that gets deeper after the first ~25 ms so the ring sounds airy, not boomy.
//  * makeVariant(): builds click variation #k from the recorded clicks: blends
//    in 10-35% of a similar recorded click (aligned on the hit), then applies
//    small pitch (+/-2%), brightness, hit-strength and ring-length changes.
//    Variation k always comes out the same; k < number of recordings = original.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

namespace scs {

constexpr int kRate = 48000;
constexpr double kPi = 3.14159265358979323846;
using Buf = std::vector<float>;

struct Biquad {
	double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
	void run(Buf& x) const {
		double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
		for (auto& v : x) {
			double y = b0 * v + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
			x2 = x1; x1 = v; y2 = y1; y1 = y;
			v = float(y);
		}
	}
	static Biquad make(double b0, double b1, double b2, double a0, double a1, double a2) {
		return {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
	}
	static Biquad highpass(double f0, double q = 0.7071) {
		double w = 2 * kPi * f0 / kRate, al = std::sin(w) / (2 * q), c = std::cos(w);
		return make((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + al, -2 * c, 1 - al);
	}
	static Biquad peak(double f0, double gainDB, double q) {
		double A = std::pow(10, gainDB / 40), w = 2 * kPi * f0 / kRate, al = std::sin(w) / (2 * q), c = std::cos(w);
		return make(1 + al * A, -2 * c, 1 - al * A, 1 + al / A, -2 * c, 1 - al / A);
	}
	static Biquad lowShelf(double f0, double gainDB, double s = 0.8) {
		double A = std::pow(10, gainDB / 40), w = 2 * kPi * f0 / kRate, c = std::cos(w);
		double al = std::sin(w) / 2 * std::sqrt((A + 1 / A) * (1 / s - 1) + 2), sa = 2 * std::sqrt(A) * al;
		return make(A * ((A + 1) - (A - 1) * c + sa), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - sa),
		            (A + 1) + (A - 1) * c + sa, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - sa);
	}
};

inline void cleanTone(Buf& x) {
	Biquad::highpass(120).run(x);
	Biquad::peak(4000, 2.0, 0.9).run(x);
	Buf hit = x, tail = x;
	Biquad::lowShelf(400, -4).run(hit);
	Biquad::lowShelf(400, -9).run(tail);
	for (size_t i = 0; i < x.size(); i++) {
		double t = double(i) / kRate;
		double m = std::clamp((t - 0.015) / 0.025, 0.0, 1.0);
		m = m * m * (3 - 2 * m);
		x[i] = float(hit[i] * (1 - m) + tail[i] * m);
	}
}

inline float rmsFirst(Buf const& x, size_t n) {
	n = std::min(n, x.size());
	double s = 0;
	for (size_t i = 0; i < n; i++) s += double(x[i]) * x[i];
	return n ? float(std::sqrt(s / n)) : 0.f;
}

// blend donor b into a at weight w, aligned on the hit, keeping a's level
inline Buf alignMix(Buf const& a, Buf const& b, float w) {
	auto peakIdx = [](Buf const& v) {
		size_t m = 0;
		for (size_t i = 0; i < std::min<size_t>(240, v.size()); i++)
			if (std::fabs(v[i]) > std::fabs(v[m])) m = i;
		return long(m);
	};
	long sh = peakIdx(a) - peakIdx(b);
	double dot = 0;
	for (long i = 0; i < 240 && i < long(a.size()); i++) {
		long j = i - sh;
		if (j >= 0 && j < long(b.size())) dot += double(a[i]) * b[j];
	}
	float pol = dot < 0 ? -1.f : 1.f;
	Buf y(a.size());
	for (long i = 0; i < long(a.size()); i++) {
		long j = i - sh;
		float bv = (j >= 0 && j < long(b.size())) ? b[j] * pol : 0.f;
		y[i] = (1 - w) * a[i] + w * bv;
	}
	float g = rmsFirst(a, 480) / (rmsFirst(y, 480) + 1e-9f);
	for (auto& v : y) v *= g;
	return y;
}

struct VariantParams {
	int donor = -1;
	float weight = 0, pitch = 1, tiltDB = 0, attack = 0, damp = 0;
};

// strength: 0 = originals only, 1 = natural, 2 = more
inline VariantParams variantParams(int k, int nBase, std::vector<float> const& baseLoud, int strength) {
	VariantParams p;
	if (k < nBase || strength <= 0) return p;
	std::mt19937 rng(uint32_t(k) * 2654435761u + 17);
	auto U = [&](float a, float b) { return std::uniform_real_distribution<float>(a, b)(rng); };
	float s = strength >= 2 ? 1.6f : 1.f;
	int base = k % nBase;
	// donor: one of the 10 recordings closest in loudness
	std::vector<std::pair<float, int>> d;
	for (int i = 0; i < nBase; i++)
		if (i != base) d.push_back({std::fabs(baseLoud[i] - baseLoud[base]), i});
	std::partial_sort(d.begin(), d.begin() + std::min<size_t>(10, d.size()), d.end());
	if (!d.empty()) {
		p.donor = d[std::uniform_int_distribution<int>(0, int(std::min<size_t>(10, d.size())) - 1)(rng)].second;
		p.weight = U(0.10f, 0.35f);
	}
	p.pitch = 1 + U(-0.008f, 0.008f) * s; // a mouse switch barely changes pitch
	p.tiltDB = U(-1.2f, 1.2f) * s;
	p.attack = U(-0.12f, 0.12f) * s;
	p.damp = U(0.f, 5.f) * s;
	return p;
}

inline Buf applyVariant(Buf const& base, Buf const* donor, VariantParams const& p) {
	Buf x = (donor && p.weight > 0) ? alignMix(base, *donor, p.weight) : base;
	if (p.pitch != 1) {
		size_t n = size_t(x.size() / p.pitch);
		Buf y(n);
		for (size_t i = 0; i < n; i++) {
			double t = i * p.pitch;
			size_t j = size_t(t);
			float f = float(t - j);
			y[i] = j + 1 < x.size() ? x[j] * (1 - f) + x[j + 1] * f : x.back();
		}
		x.swap(y);
	}
	if (p.tiltDB != 0) Biquad::lowShelf(2500, -p.tiltDB).run(x); // + = brighter
	if (p.attack != 0) {
		const size_t a = kRate * 3 / 1000, b = kRate * 6 / 1000;
		for (size_t i = 0; i < std::min(b, x.size()); i++)
			x[i] *= 1 + p.attack * (i < a ? 1.f : float(b - i) / float(b - a));
	}
	if (p.damp > 0) {
		const size_t st = kRate / 100;
		for (size_t i = st; i < x.size(); i++) x[i] *= float(std::exp(-p.damp * double(i - st) / kRate));
	}
	size_t f = std::min<size_t>(480, x.size() / 4);
	for (size_t i = 0; i < f; i++)
		x[x.size() - f + i] *= float(0.5 * (1 + std::cos(kPi * (i + 1) / f)));
	return x;
}

// A few seconds of quiet "room tone" (soft, warm hiss like a real recording's
// background), seamlessly loopable. rmsDB sets its level.
inline Buf roomTone(float seconds, float rmsDB, uint32_t seed = 7) {
	size_t n = size_t(seconds * kRate), xf = kRate / 2;
	std::mt19937 rng(seed);
	std::normal_distribution<float> nd(0, 1);
	Buf x(n + xf);
	double b0 = 0, b1 = 0, b2 = 0; // pink-ish noise (Paul Kellet's economy filter)
	for (auto& v : x) {
		double w = nd(rng);
		b0 = 0.99765 * b0 + w * 0.0990460;
		b1 = 0.96300 * b1 + w * 0.2965164;
		b2 = 0.57000 * b2 + w * 1.0526913;
		v = float(b0 + b1 + b2 + w * 0.1848);
	}
	Biquad::highpass(90).run(x);
	Biquad::lowShelf(6000, 3).run(x);          // lift everything below 6 kHz = gentle top roll-off
	Biquad::peak(9000, -6, 0.7).run(x);
	for (size_t i = 0; i < xf; i++) {          // crossfade the end into the start: seamless loop
		float f = float(i) / xf;
		x[i] = x[i] * f + x[n + i] * (1 - f);
	}
	x.resize(n);
	double s = 0;
	for (float v : x) s += double(v) * v;
	float g = float(std::pow(10, rmsDB / 20) / std::sqrt(s / n + 1e-20));
	for (auto& v : x) v *= g;
	return x;
}

} // namespace scs
