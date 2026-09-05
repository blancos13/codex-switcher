#pragma once
struct RefreshSchedule {
    static constexpr double Interval = 300.0;
    double next = 0;
    void Reset(double now) {
        next = now + Interval;
    }
    bool Due(double now, bool busy, bool modalOpen, bool hasProfiles) const {
        return hasProfiles && !busy && !modalOpen && now >= next;
    }
};
