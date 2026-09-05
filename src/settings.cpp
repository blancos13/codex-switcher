#include "studio.h"
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>

static bool Valid(const Settings &s) {
    return s.theme >= 0 && s.theme < 7 && s.font >= 0 && s.font < 3 && s.density >= 0 && s.density < 2 &&
           std::isfinite(s.scale) && s.scale >= .85f && s.scale <= 1.6f && std::isfinite(s.rounding) &&
           s.rounding >= 0 && s.rounding <= 8;
}
bool SaveSettings(const std::filesystem::path &path, const Settings &s, std::string &msg) {
    if (!Valid(s)) {
        msg = "Invalid appearance settings.";
        return false;
    }
    try {
        std::ostringstream f;
        f << "CODEX_IMGUI 1\n"
          << std::setprecision(9) << s.theme << ' ' << s.font << ' ' << s.density << ' ' << s.scale << ' '
          << s.rounding << '\n';
        accounts::WriteAtomic(path, f.str());
        msg = "Appearance saved";
        return true;
    } catch (...) {
        msg = "Could not save appearance settings.";
        return false;
    }
}
bool LoadSettings(const std::filesystem::path &path, Settings &s, std::string &msg) {
    std::ifstream f(path);
    if (!f)
        return false;
    Settings candidate;
    std::string magic;
    int version = 0;
    if (!(f >> magic >> version >> candidate.theme >> candidate.font >> candidate.density >>
          candidate.scale >> candidate.rounding) ||
        magic != "CODEX_IMGUI" || version != 1 || !Valid(candidate)) {
        msg = "Invalid appearance file; defaults loaded.";
        return false;
    }
    s = candidate;
    return true;
}
bool SettingsSelfTest() {
    auto root = std::filesystem::temp_directory_path() / accounts::Wide(accounts::Guid());
    auto path = root / L"appearance.ini";
    Settings a, b;
    a.theme = 5;
    a.font = 1;
    a.scale = 1.25f;
    std::string msg;
    bool ok = SaveSettings(path, a, msg) && LoadSettings(path, b, msg) && b.theme == 5 && b.font == 1 &&
              b.scale == 1.25f;
    accounts::WriteAtomic(path, "CODEX_IMGUI 1\n99 0 0 1 0");
    ok = ok && !LoadSettings(path, b, msg) && b.theme == 5;
    std::filesystem::remove(path);
    std::filesystem::remove(root);
    return ok;
}
