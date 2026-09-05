#pragma once
#include <string>
#include <cstdint>

std::string RelativeTime(const std::string &isoTimestamp);
std::string RelativeTimeAt(const std::string &isoTimestamp, int64_t nowUtcSeconds);
bool RelativeTimeSelfTest();
bool ParseUtcTimestamp(const std::string &timestamp, int64_t &utcSeconds);
std::string ResetCountdownAt(int64_t resetsAt, int64_t nowUtcSeconds);
std::string ResetCountdown(int64_t resetsAt);
