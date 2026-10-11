#pragma once

#include <cstdint>

namespace f2c_cpp {

// Stage 6 top-bar signal pill. This is the radio, not LinkHealthMonitor:
// one SNMP RSSI sample from the laptop-side Microhard, classified with
// the same bands Stage 4 already uses.
//
//   >= -65 dBm  SIGNAL STRONG
//   >= -80 dBm  SIGNAL WEAK
//   below that  NO LINK
//
// A parsed 0 is not a strong signal. The radio returns 0 when it has no
// peer, and 0 would otherwise fall on the strong side of -65. A failed
// get does not erase a sample that is still inside kRadioFreshMs; once
// that window passes, or if there was never a usable sample, the pill
// is NO LINK. Before the first completed probe of a visit it is unknown
// (SIGNAL —), so a cold start does not flash red.
//
// NoConfig is not a probe result. The screen shows it when this robot
// has no radio IP or RSSI OID, so a probe that cannot start is not
// painted as "still waiting".

enum class RadioLinkPill {
    Unknown,
    Strong,
    Weak,
    NoLink,
    NoConfig,
};

struct RadioLinkSample {
    int rssi_dbm = 0;
    // Wall-clock ms of the last non-zero reading. 0 means there is no
    // reading to hold.
    int64_t wall_ms = 0;
    bool probed = false;
};

inline constexpr int kRadioStrongMinDbm = -65;
inline constexpr int kRadioWeakMinDbm = -80;
inline constexpr int64_t kRadioFreshMs = 5000;

inline void applyRadioProbe(RadioLinkSample* sample, bool ok, int rssi_dbm,
                            int64_t wall_ms) {
    if (!sample) {
        return;
    }
    sample->probed = true;
    if (ok && rssi_dbm != 0) {
        sample->rssi_dbm = rssi_dbm;
        sample->wall_ms = wall_ms;
        return;
    }
    if (ok && rssi_dbm == 0) {
        // Explicit "no peer". Drop the held sample so a 0 does not sit
        // behind a strength from a second ago for the rest of the window.
        sample->rssi_dbm = 0;
        sample->wall_ms = 0;
    }
    // A failed get leaves the last good sample alone. Freshness ages it out.
}

inline RadioLinkPill classifyRadioLink(const RadioLinkSample& sample,
                                       int64_t now_ms) {
    if (!sample.probed) {
        return RadioLinkPill::Unknown;
    }
    const bool fresh = sample.wall_ms > 0 &&
                       (now_ms - sample.wall_ms) <= kRadioFreshMs &&
                       sample.rssi_dbm != 0;
    if (!fresh) {
        return RadioLinkPill::NoLink;
    }
    if (sample.rssi_dbm >= kRadioStrongMinDbm) {
        return RadioLinkPill::Strong;
    }
    if (sample.rssi_dbm >= kRadioWeakMinDbm) {
        return RadioLinkPill::Weak;
    }
    return RadioLinkPill::NoLink;
}

inline const char* radioLinkLabel(RadioLinkPill pill) {
    switch (pill) {
        case RadioLinkPill::Strong:
            return "SIGNAL STRONG";
        case RadioLinkPill::Weak:
            return "SIGNAL WEAK";
        case RadioLinkPill::NoLink:
            return "NO LINK";
        case RadioLinkPill::NoConfig:
            return "NO RADIO CONFIG";
        case RadioLinkPill::Unknown:
            return "SIGNAL —";
    }
    return "SIGNAL —";
}

}  // namespace f2c_cpp
