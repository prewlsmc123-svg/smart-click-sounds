#pragma once
// Turns the engine's decisions into audio: holds the recorded (tone-cleaned)
// sounds and renders click variation #k on demand. Shared by the mod and the
// offline test renderer, so a preview render is exactly what the mod plays.

#include "dsp.hpp"
#include "engine.hpp"

namespace scs {

struct Pack {
	// decoded mono 48 kHz audio, same order as engine.clicks/releases/softClicks/softReleases
	std::vector<Buf> pcm[4];

	std::vector<Sound>& list(Engine& e, int i) {
		switch (i) {
		case 0: return e.clicks;
		case 1: return e.releases;
		case 2: return e.softClicks;
		default: return e.softReleases;
		}
	}

	static std::vector<float> louds(std::vector<Sound> const& v) {
		std::vector<float> l;
		for (auto& s : v) l.push_back(s.loud);
		return l;
	}

	// strength: 0 off, 1 natural, 2 more
	Buf render(Engine& e, Play const& p, int strength) {
		auto& snd = list(e, p.list);
		auto& pcmL = pcm[p.list];
		if (p.base < 0 || p.base >= int(pcmL.size())) return {};
		bool isRelease = p.list == 1 || p.list == 3;
		if (!isRelease) { // a press: variation of a recorded click
			auto prm = variantParams(p.variation, int(snd.size()), louds(snd), strength);
			Buf const* donor = prm.donor >= 0 ? &pcmL[prm.donor] : nullptr;
			return applyVariant(pcmL[p.base], donor, prm);
		}
		auto& presses = list(e, p.pressList);
		if (p.variation == p.pressVariation && !presses.empty()) {
			// its own release: varied exactly like its press (same donor click's release)
			auto prm = variantParams(p.pressVariation, int(presses.size()), louds(presses), strength);
			Buf const* donor = nullptr;
			if (prm.donor >= 0 && presses[prm.donor].partner >= 0 && presses[prm.donor].partner < int(pcmL.size()))
				donor = &pcmL[presses[prm.donor].partner];
			else
				prm.weight = 0;
			return applyVariant(pcmL[p.base], donor, prm);
		}
		auto prm = variantParams(p.variation, int(snd.size()), louds(snd), strength);
		Buf const* donor = prm.donor >= 0 ? &pcmL[prm.donor] : nullptr;
		return applyVariant(pcmL[p.base], donor, prm);
	}
};

} // namespace scs
