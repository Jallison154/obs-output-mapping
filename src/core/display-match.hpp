/*
Output Mapping for OBS Studio
Copyright (C) 2026 Okami Designs

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// Safe matching of saved display identities against connected displays.
// Pure C++ so it can be unit tested (tests/test_core.cpp).
//
// Rule of thumb: only report Connected when we are certain it is the same
// physical display. Anything weaker is Ambiguous (needs the user to
// reassign) so fullscreen output never lands on the wrong screen.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace om {

struct DisplayIdentity {
	std::string uuid;     /* OS stable id (macOS: CGDisplayCreateUUIDFromDisplayID) */
	uint32_t vendor = 0;  /* EDID vendor id (0 = unknown)  */
	uint32_t model = 0;   /* EDID product id (0 = unknown) */
	uint32_t serial = 0;  /* EDID serial (0 = unknown / not reported) */
	std::string name;     /* human readable, e.g. "DELL U2720Q" */

	bool empty() const { return uuid.empty() && vendor == 0 && model == 0 && name.empty(); }
};

enum class MatchStatus {
	Connected,        /* exact OS identity match                          */
	ConnectedSerial,  /* OS id changed (e.g. other port) but unique EDID
			     vendor+model+serial match – same physical unit     */
	Ambiguous,        /* plausible candidates but not provably the same  */
	Disconnected,     /* nothing plausible connected                     */
	Unassigned,       /* mapping has no physical identity                */
};

inline bool isConnected(MatchStatus s)
{
	return s == MatchStatus::Connected || s == MatchStatus::ConnectedSerial;
}

struct MatchResult {
	MatchStatus status = MatchStatus::Disconnected;
	int index = -1;              /* index into `connected` when isConnected() */
	std::vector<int> candidates; /* for Ambiguous: plausible displays         */
};

inline MatchResult matchDisplay(const DisplayIdentity &saved, const std::vector<DisplayIdentity> &connected)
{
	MatchResult res;
	if (saved.empty()) {
		res.status = MatchStatus::Unassigned;
		return res;
	}

	/* 1. Exact OS identity. Must be unique to count. */
	if (!saved.uuid.empty()) {
		std::vector<int> hits;
		for (int i = 0; i < (int)connected.size(); i++)
			if (connected[i].uuid == saved.uuid)
				hits.push_back(i);
		if (hits.size() == 1) {
			res.status = MatchStatus::Connected;
			res.index = hits[0];
			return res;
		}
		if (hits.size() > 1) {
			res.status = MatchStatus::Ambiguous;
			res.candidates = hits;
			return res;
		}
	}

	/* 2. Same make/model. A unique non-zero serial proves identity. */
	std::vector<int> sameModel;
	if (saved.vendor != 0 || saved.model != 0) {
		for (int i = 0; i < (int)connected.size(); i++)
			if (connected[i].vendor == saved.vendor && connected[i].model == saved.model)
				sameModel.push_back(i);
	}

	if (saved.serial != 0) {
		std::vector<int> sameSerial;
		for (int i : sameModel)
			if (connected[i].serial == saved.serial)
				sameSerial.push_back(i);
		if (sameSerial.size() == 1) {
			res.status = MatchStatus::ConnectedSerial;
			res.index = sameSerial[0];
			return res;
		}
		if (sameSerial.size() > 1) {
			res.status = MatchStatus::Ambiguous;
			res.candidates = sameSerial;
			return res;
		}
		/* Same model with a *different* serial is a different unit. */
		for (int i : sameModel)
			if (connected[i].serial == 0)
				res.candidates.push_back(i);
		res.status = res.candidates.empty() ? MatchStatus::Disconnected : MatchStatus::Ambiguous;
		return res;
	}

	/* 3. No serial to prove identity: any same-model display is only a
	 *    guess (could be a twin unit or a different port). */
	if (!sameModel.empty()) {
		res.status = MatchStatus::Ambiguous;
		res.candidates = sameModel;
		return res;
	}

	/* 4. No hardware ids at all (virtual/indirect displays): a display with
	 *    the same name is a candidate, never a proof. */
	if (saved.vendor == 0 && saved.model == 0 && !saved.name.empty()) {
		for (int i = 0; i < (int)connected.size(); i++)
			if (connected[i].name == saved.name)
				res.candidates.push_back(i);
		if (!res.candidates.empty()) {
			res.status = MatchStatus::Ambiguous;
			return res;
		}
	}

	res.status = MatchStatus::Disconnected;
	return res;
}

inline const char *matchStatusName(MatchStatus s)
{
	switch (s) {
	case MatchStatus::Connected:
		return "connected";
	case MatchStatus::ConnectedSerial:
		return "connected-serial";
	case MatchStatus::Ambiguous:
		return "ambiguous";
	case MatchStatus::Disconnected:
		return "disconnected";
	case MatchStatus::Unassigned:
		return "unassigned";
	}
	return "unknown";
}

} // namespace om
