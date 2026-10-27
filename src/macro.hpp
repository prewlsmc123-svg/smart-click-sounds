#pragma once
// Reads GD macros for the built-in macro player (plain C++, testable outside GD):
//  * GDR (.gdr / .gdr.json): xdBot, Mega Hack v8, Eclipse... as MessagePack or
//    JSON. Includes xdBot's per-frame position fixes when present.
//  * GDR2 (.gdr2): Mega Hack's newer binary format (inputs only).

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace scs::macro {

struct Input {
	int frame = 0;
	int button = 1;      // 1 jump, 2 left, 3 right
	bool player2 = false; // as stored: xdBot's raw "2p" flag
	bool down = false;
};

struct PlayerFix {
	bool has = false;
	float x = 0, y = 0, r = 0;
	bool hasR = false;
};

struct Fix {
	int frame = 0;
	PlayerFix p1, p2;
};

struct Macro {
	std::string format, bot, author, levelName;
	int64_t levelId = 0;
	double framerate = 240;
	// xdBot counts frames as int(levelTime * framerate) + 1; other bots use the
	// game's tick counter (m_currentProgress)
	bool timeFrames = false;
	bool platformer = false;
	std::vector<Input> inputs;
	std::vector<Fix> fixes;
	int presses() const {
		int n = 0;
		for (auto& i : inputs) n += i.down;
		return n;
	}
};

// ---- a tiny generic value tree for MessagePack / JSON ----
struct Value {
	enum Type { Nil, Bool, Int, Num, Str, Arr, Map } type = Nil;
	bool b = false;
	int64_t i = 0;
	double d = 0;
	std::string s;
	std::vector<Value> a;
	std::vector<std::string> keys; // map keys, parallel to vals
	std::vector<Value> vals;

	Value const* get(char const* key) const {
		if (type != Map) return nullptr;
		for (size_t j = 0; j < keys.size(); j++)
			if (keys[j] == key) return &vals[j];
		return nullptr;
	}
	double num(double def = 0) const {
		switch (type) {
		case Int: return double(i);
		case Num: return d;
		case Bool: return b ? 1 : 0;
		default: return def;
		}
	}
	bool truthy() const { return type == Bool ? b : num() != 0; }
	std::string str() const { return type == Str ? s : std::string(); }
};

class MsgPack {
public:
	MsgPack(uint8_t const* p, size_t n) : p_(p), n_(n) {}
	bool ok = true;
	Value read(int depth = 0) {
		Value v;
		if (depth > 64 || !need(1)) return fail();
		uint8_t c = p_[pos_++];
		if (c <= 0x7f) { v.type = Value::Int; v.i = c; return v; }
		if (c >= 0xe0) { v.type = Value::Int; v.i = int8_t(c); return v; }
		if ((c & 0xf0) == 0x80) return map(c & 0x0f, depth);
		if ((c & 0xf0) == 0x90) return arr(c & 0x0f, depth);
		if ((c & 0xe0) == 0xa0) return string(c & 0x1f);
		switch (c) {
		case 0xc0: return v;
		case 0xc2: v.type = Value::Bool; v.b = false; return v;
		case 0xc3: v.type = Value::Bool; v.b = true; return v;
		case 0xc4: return bin(uint8_t(be(1)));
		case 0xc5: return bin(uint16_t(be(2)));
		case 0xc6: return bin(uint32_t(be(4)));
		case 0xca: { uint32_t u = uint32_t(be(4)); float f; std::memcpy(&f, &u, 4); v.type = Value::Num; v.d = f; return v; }
		case 0xcb: { uint64_t u = be(8); double f; std::memcpy(&f, &u, 8); v.type = Value::Num; v.d = f; return v; }
		case 0xcc: v.type = Value::Int; v.i = int64_t(be(1)); return v;
		case 0xcd: v.type = Value::Int; v.i = int64_t(be(2)); return v;
		case 0xce: v.type = Value::Int; v.i = int64_t(be(4)); return v;
		case 0xcf: v.type = Value::Int; v.i = int64_t(be(8)); return v;
		case 0xd0: v.type = Value::Int; v.i = int8_t(be(1)); return v;
		case 0xd1: v.type = Value::Int; v.i = int16_t(be(2)); return v;
		case 0xd2: v.type = Value::Int; v.i = int32_t(be(4)); return v;
		case 0xd3: v.type = Value::Int; v.i = int64_t(be(8)); return v;
		case 0xd9: return string(size_t(be(1)));
		case 0xda: return string(size_t(be(2)));
		case 0xdb: return string(size_t(be(4)));
		case 0xdc: return arr(size_t(be(2)), depth);
		case 0xdd: return arr(size_t(be(4)), depth);
		case 0xde: return map(size_t(be(2)), depth);
		case 0xdf: return map(size_t(be(4)), depth);
		case 0xd4: skip(2); return v; // fixext
		case 0xd5: skip(3); return v;
		case 0xd6: skip(5); return v;
		case 0xd7: skip(9); return v;
		case 0xd8: skip(17); return v;
		case 0xc7: { size_t n = size_t(be(1)); skip(n + 1); return v; }
		case 0xc8: { size_t n = size_t(be(2)); skip(n + 1); return v; }
		case 0xc9: { size_t n = size_t(be(4)); skip(n + 1); return v; }
		default: return fail();
		}
	}

private:
	uint8_t const* p_;
	size_t n_, pos_ = 0;
	Value fail() { ok = false; return {}; }
	bool need(size_t k) {
		if (pos_ + k > n_) { ok = false; return false; }
		return true;
	}
	uint64_t be(int k) {
		if (!need(size_t(k))) return 0;
		uint64_t u = 0;
		for (int j = 0; j < k; j++) u = (u << 8) | p_[pos_++];
		return u;
	}
	void skip(size_t k) { if (need(k)) pos_ += k; }
	Value string(size_t k) {
		Value v;
		if (!need(k)) return fail();
		v.type = Value::Str;
		v.s.assign(reinterpret_cast<char const*>(p_ + pos_), k);
		pos_ += k;
		return v;
	}
	Value bin(size_t k) { skip(k); return {}; }
	Value arr(size_t k, int depth) {
		Value v;
		v.type = Value::Arr;
		if (k > n_) return fail();
		v.a.reserve(k);
		for (size_t j = 0; j < k && ok; j++) v.a.push_back(read(depth + 1));
		return v;
	}
	Value map(size_t k, int depth) {
		Value v;
		v.type = Value::Map;
		if (k > n_) return fail();
		for (size_t j = 0; j < k && ok; j++) {
			Value key = read(depth + 1);
			Value val = read(depth + 1);
			std::string ks = key.type == Value::Str ? key.s : std::to_string(key.i);
			v.keys.push_back(std::move(ks));
			v.vals.push_back(std::move(val));
		}
		return v;
	}
};

class Json {
public:
	explicit Json(std::string const& s) : s_(s) {}
	bool ok = true;
	Value read(int depth = 0) {
		ws();
		Value v;
		if (depth > 64 || pos_ >= s_.size()) return fail();
		char c = s_[pos_];
		if (c == '{') {
			pos_++;
			v.type = Value::Map;
			ws();
			if (peek('}')) { pos_++; return v; }
			while (ok) {
				ws();
				Value k = read(depth + 1);
				ws();
				if (!peek(':')) return fail();
				pos_++;
				Value val = read(depth + 1);
				v.keys.push_back(k.s);
				v.vals.push_back(std::move(val));
				ws();
				if (peek(',')) { pos_++; continue; }
				if (peek('}')) { pos_++; break; }
				return fail();
			}
			return v;
		}
		if (c == '[') {
			pos_++;
			v.type = Value::Arr;
			ws();
			if (peek(']')) { pos_++; return v; }
			while (ok) {
				v.a.push_back(read(depth + 1));
				ws();
				if (peek(',')) { pos_++; continue; }
				if (peek(']')) { pos_++; break; }
				return fail();
			}
			return v;
		}
		if (c == '"') {
			pos_++;
			v.type = Value::Str;
			while (pos_ < s_.size() && s_[pos_] != '"') {
				if (s_[pos_] == '\\' && pos_ + 1 < s_.size()) {
					char e = s_[++pos_];
					if (e == 'u') { pos_ += 4; v.s += '?'; }
					else v.s += e == 'n' ? '\n' : e == 't' ? '\t' : e;
					pos_++;
				} else {
					v.s += s_[pos_++];
				}
			}
			pos_++;
			return v;
		}
		if (s_.compare(pos_, 4, "true") == 0) { pos_ += 4; v.type = Value::Bool; v.b = true; return v; }
		if (s_.compare(pos_, 5, "false") == 0) { pos_ += 5; v.type = Value::Bool; return v; }
		if (s_.compare(pos_, 4, "null") == 0) { pos_ += 4; return v; }
		size_t st = pos_;
		bool isFloat = false;
		while (pos_ < s_.size() && (std::isdigit(uint8_t(s_[pos_])) || std::strchr("+-.eE", s_[pos_]))) {
			if (std::strchr(".eE", s_[pos_])) isFloat = true;
			pos_++;
		}
		if (st == pos_) return fail();
		std::string t = s_.substr(st, pos_ - st);
		if (isFloat) { v.type = Value::Num; v.d = std::strtod(t.c_str(), nullptr); }
		else { v.type = Value::Int; v.i = std::strtoll(t.c_str(), nullptr, 10); }
		return v;
	}

private:
	std::string const& s_;
	size_t pos_ = 0;
	Value fail() { ok = false; return {}; }
	bool peek(char c) const { return pos_ < s_.size() && s_[pos_] == c; }
	void ws() { while (pos_ < s_.size() && std::isspace(uint8_t(s_[pos_]))) pos_++; }
};

inline PlayerFix readPlayerFix(Value const* v) {
	PlayerFix f;
	if (!v || v->type != Value::Map) return f;
	auto x = v->get("x"), y = v->get("y"), r = v->get("r");
	if (!x || !y) return f;
	f.has = true;
	f.x = float(x->num());
	f.y = float(y->num());
	if (r) { f.hasR = true; f.r = float(r->num()); }
	return f;
}

inline std::optional<Macro> fromGDR(Value const& root, std::string& err) {
	if (root.type != Value::Map) { err = "not a GDR macro"; return std::nullopt; }
	Macro m;
	m.format = "GDR";
	if (auto b = root.get("bot")) {
		if (auto n = b->get("name")) m.bot = n->str();
	}
	if (auto a = root.get("author")) m.author = a->str();
	if (auto l = root.get("level")) {
		if (auto id = l->get("id")) m.levelId = int64_t(id->num());
		if (auto n = l->get("name")) m.levelName = n->str();
	}
	if (auto f = root.get("framerate")) m.framerate = f->num(240);
	if (!(m.framerate > 0)) m.framerate = 240;
	if (auto p = root.get("platformer")) m.platformer = p->truthy();
	m.timeFrames = m.bot == "xdBot";
	auto ins = root.get("inputs");
	if (!ins || ins->type != Value::Arr) { err = "the macro has no inputs"; return std::nullopt; }
	for (auto& v : ins->a) {
		auto fr = v.get("frame");
		if (!fr || fr->type == Value::Nil) continue;
		Input in;
		in.frame = int(fr->num());
		if (auto b = v.get("btn")) in.button = int(b->num(1));
		if (auto p = v.get("2p")) in.player2 = p->truthy();
		if (auto d = v.get("down")) in.down = d->truthy();
		m.inputs.push_back(in);
	}
	if (auto fx = root.get("frameFixes"); fx && fx->type == Value::Arr) {
		for (auto& v : fx->a) {
			auto fr = v.get("frame");
			if (!fr) continue;
			Fix f;
			f.frame = int(fr->num());
			f.p1 = readPlayerFix(v.get("p1"));
			f.p2 = readPlayerFix(v.get("p2"));
			if (f.p1.has || f.p2.has) m.fixes.push_back(f);
		}
	}
	std::stable_sort(m.inputs.begin(), m.inputs.end(), [](Input const& a, Input const& b) { return a.frame < b.frame; });
	std::stable_sort(m.fixes.begin(), m.fixes.end(), [](Fix const& a, Fix const& b) { return a.frame < b.frame; });
	return m;
}

// Mega Hack's GDR2
inline std::optional<Macro> fromGDR2(std::vector<uint8_t> const& d, std::string& err) {
	size_t pos = 3;
	bool bad = false;
	auto byte = [&]() -> uint8_t {
		if (pos >= d.size()) { bad = true; return 0; }
		return d[pos++];
	};
	auto varint = [&]() -> uint64_t {
		uint64_t v = 0;
		for (int i = 0; i < 10; i++) {
			uint8_t c = byte();
			v |= uint64_t(c & 0x7f) << (7 * i);
			if (!(c & 0x80)) return v;
		}
		bad = true;
		return 0;
	};
	auto bytes = [&](size_t n) {
		std::string s;
		if (pos + n > d.size()) { bad = true; pos = d.size(); return s; }
		s.assign(reinterpret_cast<char const*>(d.data() + pos), n);
		pos += n;
		return s;
	};
	auto str = [&]() { return bytes(size_t(varint())); };
	Macro m;
	m.format = "GDR2";
	if (varint() != 2) { err = "unsupported GDR2 version"; return std::nullopt; }
	std::string tag = str();
	m.author = str();
	str(); // description
	bytes(4); // duration
	varint(); // game version
	bytes(8); // framerate (Mega Hack writes the display FPS here; frames are game ticks)
	varint(); // seed
	varint(); // coins
	byte();   // ldm
	m.platformer = byte() != 0;
	m.bot = str();
	varint(); // bot version
	m.levelId = int64_t(varint());
	m.levelName = str();
	bytes(size_t(varint())); // extension
	uint64_t deaths = varint(), lastDeath = 0;
	for (uint64_t i = 0; i < deaths && !bad; i++) lastDeath += varint();
	uint64_t total = varint(), p1 = varint();
	if (bad || total > d.size()) { err = "couldn't read the GDR2 header"; return std::nullopt; }
	uint64_t prev = 0;
	for (uint64_t i = 0; i < total && !bad; i++) {
		if (i == p1) prev = 0;
		uint64_t packed = varint();
		uint64_t delta = packed >> 1, btn = 1;
		if (m.platformer) { delta = packed >> 3; btn = (packed >> 1) & 3; }
		uint64_t frame = prev + delta;
		prev = frame;
		if (!tag.empty()) bytes(size_t(varint()));
		if (frame < lastDeath) continue; // keep the final attempt
		Input in;
		in.frame = int(frame - lastDeath);
		in.button = int(btn);
		in.player2 = i >= p1;
		in.down = packed & 1;
		m.inputs.push_back(in);
	}
	if (bad && m.inputs.empty()) { err = "couldn't read the GDR2 inputs"; return std::nullopt; }
	std::stable_sort(m.inputs.begin(), m.inputs.end(), [](Input const& a, Input const& b) { return a.frame < b.frame; });
	return m;
}

inline std::optional<Macro> parse(std::vector<uint8_t> const& d, std::string& err) {
	if (d.size() >= 4 && d[0] == 'G' && d[1] == 'D' && d[2] == 'R') return fromGDR2(d, err);
	size_t i = 0;
	while (i < d.size() && std::isspace(d[i])) i++;
	if (i < d.size() && d[i] == '{') {
		std::string s(d.begin(), d.end());
		Json j(s);
		Value v = j.read();
		if (!j.ok) { err = "couldn't read the JSON macro"; return std::nullopt; }
		return fromGDR(v, err);
	}
	MsgPack mp(d.data(), d.size());
	Value v = mp.read();
	if (!mp.ok) { err = "couldn't read the macro file"; return std::nullopt; }
	return fromGDR(v, err);
}

template <class Path>
inline std::optional<Macro> load(Path const& path, std::string& err) {
	std::ifstream f(path, std::ios::binary);
	if (!f) { err = "couldn't open the file"; return std::nullopt; }
	std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), {});
	if (d.empty()) { err = "the file is empty"; return std::nullopt; }
	return parse(d, err);
}

} // namespace scs::macro
