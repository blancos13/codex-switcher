
#include "relative_time.h"
#include <chrono>
#include <cstdio>
#include <algorithm>
#include <cctype>

namespace {
bool Parse(const std::string &text, int64_t &result) {
    if (text.size() < 20)
        return false;
    int y, m, d, h, min, sec;
    if (std::sscanf(text.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &y, &m, &d, &h, &min, &sec) != 6)
        return false;
    if (y < 1970 || y > 9999 || m < 1 || m > 12 || d < 1 || d > 31 || h < 0 || h > 23 || min < 0 ||
        min > 59 || sec < 0 || sec > 59)
        return false;
    const bool leap=y%4==0&&(y%100!=0||y%400==0);
    const int monthDays[]={31,leap?29:28,31,30,31,30,31,31,30,31,30,31};
    if(d>monthDays[m-1])return false;
    // Gregorian civil date -> UTC seconds, independent of timezone and platform time_t.
    int year=y-(m<=2),era=year/400;unsigned yoe=unsigned(year-era*400);
    unsigned shifted=unsigned(m+(m>2?-3:9));
    unsigned doy=(153*shifted+2)/5+unsigned(d)-1;
    unsigned doe=yoe*365+yoe/4-yoe/100+doy;
    int64_t days=int64_t(era)*146097+doe-719468;
    result=days*86400+h*3600+min*60+sec;
    size_t pos = 19;
    if (pos < text.size() && text[pos] == '.') {
        ++pos;
        size_t start = pos;
        while (pos < text.size() && isdigit(static_cast<unsigned char>(text[pos])))
            ++pos;
        if (pos == start)
            return false;
    }
    if (pos + 1 == text.size() && text[pos] == 'Z')
        return true;
    if (pos + 6 != text.size() || (text[pos] != '+' && text[pos] != '-') || text[pos + 3] != ':')
        return false;
    for (size_t index : {pos + 1, pos + 2, pos + 4, pos + 5})
        if (!isdigit(static_cast<unsigned char>(text[index])))
            return false;
    int hours = (text[pos + 1] - '0') * 10 + text[pos + 2] - '0',
        minutes = (text[pos + 4] - '0') * 10 + text[pos + 5] - '0';
    if (hours > 14 || minutes > 59 || (hours == 14 && minutes != 0))
        return false;
    result -= (text[pos] == '+' ? 1 : -1) * (hours * 3600 + minutes * 60);
    return true;
}
} // namespace
bool ParseUtcTimestamp(const std::string &timestamp, int64_t &utcSeconds) {
    return Parse(timestamp, utcSeconds);
}
std::string ResetCountdownAt(int64_t resetsAt, int64_t now) {
    if (resetsAt <= 0)
        return "Reset time unavailable";
    if (resetsAt <= now)
        return "Reset time reached. Refresh usage to update.";
    int64_t remaining = resetsAt - now;
    auto unit = [](int64_t n, const char *name) {
        return std::to_string(n) + " " + name + (n == 1 ? "" : "s");
    };
    std::string text;
    if (remaining >= 86400) {
        text = unit(remaining / 86400, "day");
        if (remaining % 86400 >= 3600)
            text += ", " + unit(remaining % 86400 / 3600, "hour");
    } else if (remaining >= 3600) {
        text = unit(remaining / 3600, "hour");
        if (remaining % 3600 >= 60)
            text += ", " + unit(remaining % 3600 / 60, "minute");
    } else if (remaining >= 60) {
        text = unit(remaining / 60, "minute");
        if (remaining % 60)
            text += ", " + unit(remaining % 60, "second");
    } else
        text = unit(remaining, "second");
    return "Resets in " + text;
}
std::string ResetCountdown(int64_t resetsAt) {
    return ResetCountdownAt(resetsAt, std::chrono::duration_cast<std::chrono::seconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
}
std::string RelativeTimeAt(const std::string &timestamp, int64_t now) {
    int64_t checked = 0;
    if (!Parse(timestamp, checked))
        return "Not checked yet";
    int64_t elapsed = std::max<int64_t>(0, now - checked);
    if (elapsed < 1)
        return "Just now";
    auto ago = [](int64_t n, const char *unit) {
        return std::to_string(n) + " " + unit + (n == 1 ? " ago" : "s ago");
    };
    if (elapsed < 60)
        return ago(elapsed, "second");
    if (elapsed < 3600)
        return ago(elapsed / 60, "minute");
    if (elapsed < 86400)
        return ago(elapsed / 3600, "hour");
    return ago(elapsed / 86400, "day");
}
std::string RelativeTime(const std::string &timestamp) {
    return RelativeTimeAt(timestamp, std::chrono::duration_cast<std::chrono::seconds>(
                                         std::chrono::system_clock::now().time_since_epoch())
                                         .count());
}
bool RelativeTimeSelfTest() {
    int64_t base = 0;
    if (!Parse("2026-09-05T12:00:00Z", base))
        return false;
    return RelativeTimeAt("2026-09-05T12:00:00Z", base + 10) == "10 seconds ago" &&
           RelativeTimeAt("2026-09-05T12:00:00.1234567+00:00", base + 1800) == "30 minutes ago" &&
           RelativeTimeAt("2026-09-05T15:00:00+03:00", base + 3600) == "1 hour ago" &&
           RelativeTimeAt("2026-09-05T12:00:00Z", base + 86400) == "1 day ago" &&
           RelativeTimeAt("2026-09-05T12:00:00Z", base + 1) == "1 second ago" &&
           RelativeTimeAt("2026-09-05T12:00:00Z", base + 60) == "1 minute ago" &&
           RelativeTimeAt("2026-09-05T12:00:01Z", base) == "Just now" &&
           RelativeTimeAt("invalid", base) == "Not checked yet" &&
           RelativeTimeAt("2026-09-05T12:00:00+99:00", base) == "Not checked yet" &&
           ResetCountdownAt(base + 10, base) == "Resets in 10 seconds" &&
           ResetCountdownAt(base + 7260, base) == "Resets in 2 hours, 1 minute" &&
           ResetCountdownAt(base + 90000, base) == "Resets in 1 day, 1 hour" &&
           ResetCountdownAt(base, base) == "Reset time reached. Refresh usage to update." &&
           ResetCountdownAt(0, base) == "Reset time unavailable";
}
