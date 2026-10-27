// prints what the macro reader sees: macro_test file...
#include "../src/macro.hpp"
#include <algorithm>
#include <cstdio>
int main(int argc, char** argv) {
	for (int k = 1; k < argc; k++) {
		std::string err;
		auto m = scs::macro::load(argv[k], err);
		if (!m) { std::printf("%s: ERROR %s\n", argv[k], err.c_str()); continue; }
		std::printf("%s: %s bot=%s author=%s level=%s (%lld) fps=%.0f timeFrames=%d inputs=%zu presses=%d fixes=%zu",
			argv[k], m->format.c_str(), m->bot.c_str(), m->author.c_str(), m->levelName.c_str(), (long long)m->levelId,
			m->framerate, m->timeFrames, m->inputs.size(), m->presses(), m->fixes.size());
		if (!m->inputs.empty()) std::printf(" first=%d/%d last=%d", m->inputs[0].frame, m->inputs[0].down, m->inputs.back().frame);
		if (!m->fixes.empty()) std::printf(" fix0=(%d %.3f %.3f r%d)", m->fixes[0].frame, m->fixes[0].p1.x, m->fixes[0].p1.y, m->fixes[0].p1.hasR);
		std::printf("\n");
	}
}
