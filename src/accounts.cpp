#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wincrypt.h>
#else
#include <openssl/evp.h>
#include <openssl/sha.h>
#endif
#include "platform.h"
#include "accounts.h"
#include "relative_time.h"
#include <algorithm>

#include <cmath>
#include <fstream>
#include <set>


#include <sstream>


namespace accounts {
static void Fail(const char *s) {
    throw std::runtime_error(s);
}
#ifdef _WIN32
std::wstring Wide(const std::string &s) {
    if (s.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), int(s.size()), nullptr, 0);
    std::wstring r(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), r.data(), n);
    return r;
}
std::string Utf8(const std::wstring &s) {
    if (s.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0, nullptr, nullptr);
    std::string r(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), r.data(), n, nullptr, nullptr);
    return r;
}
static Path Folder(REFKNOWNFOLDERID id) {
    PWSTR value = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &value)))
        Fail("Windows user folder is unavailable.");
    Path p(value);
    CoTaskMemFree(value);
    return p;
}
Path UserHome() {
    return Folder(FOLDERID_Profile);
}
Path ActiveAuth() {
    wchar_t value[32768];
    DWORD n = GetEnvironmentVariableW(L"CODEX_HOME", value, 32768);
    return (n > 0 && n < 32768 ? Path(value) : UserHome() / L".codex") / L"auth.json";
}
std::string Guid() {
    GUID g;
    if (FAILED(CoCreateGuid(&g)))
        Fail("Could not create a profile ID.");
    wchar_t raw[40];
    StringFromGUID2(g, raw, 40);
    std::string result;
    for (auto c : std::wstring(raw))
        if (iswxdigit(c))
            result += char(towlower(c));
    return result;
}
std::string Timestamp() {
    SYSTEMTIME t;
    GetSystemTime(&t);
    char s[32];
    sprintf_s(s, "%04d-%02d-%02dT%02d:%02d:%02dZ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return s;
}
std::string Read(const Path &p) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f)
        Fail("Could not read the local file.");
    auto size = f.tellg();
    if (size < 0 || size > 8 * 1024 * 1024)
        Fail("Local file has an invalid size.");
    std::string data(size_t(size), '\0');
    f.seekg(0);
    if (!f.read(data.data(), size))
        Fail("Could not read the local file.");
    return data;
}
void WriteAtomic(const Path &path, const std::string &data) {
    std::filesystem::create_directories(path.parent_path());
    auto temp = path;
    temp += L"." + Wide(Guid()) + L".tmp";
    HANDLE f = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        Fail("Could not create a local file. Check folder permissions.");
    DWORD written = 0;
    bool ok = WriteFile(f, data.data(), DWORD(data.size()), &written, nullptr) && written == data.size() &&
              FlushFileBuffers(f);
    CloseHandle(f);
    if (!ok || !MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        platform::RemoveFile(temp);
        Fail("Could not save local data.");
    }
}
#endif
static bool ValidId(const std::string &s) {
    return s.size() == 32 &&
           std::all_of(s.begin(), s.end(), [](unsigned char c) { return isxdigit(c) != 0; });
}
static std::string Label(std::string s) {
    auto a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    if (a == std::string::npos)
        Fail("Enter a name for this account.");
    s = s.substr(a, b - a + 1);
    if (Wide(s).size() > 40)
        Fail("Account names must be at most 40 characters.");
    return s;
}
static std::string Color(const std::string &s) {
    return s.size() == 7 && s[0] == '#' &&
                   std::all_of(s.begin() + 1, s.end(), [](unsigned char c) { return isxdigit(c) != 0; })
               ? s
               : "#4296FA";
}
static std::string AccountId(const Json &j) {
    if (j.is_object())
        for (auto it = j.begin(); it != j.end(); ++it) {
            std::string k = it.key();
            std::transform(k.begin(), k.end(), k.begin(), [](unsigned char c) { return char(tolower(c)); });
            if ((k == "account_id" || k == "accountid" || k == "chatgpt_account_id" ||
                 k == "chatgptaccountid") &&
                it.value().is_string() && !it.value().get<std::string>().empty())
                return it.value().get<std::string>();
            auto nested = AccountId(it.value());
            if (!nested.empty())
                return nested;
        }
    else if (j.is_array())
        for (auto &item : j) {
            auto nested = AccountId(item);
            if (!nested.empty())
                return nested;
        }
    return {};
}
std::string Fingerprint(const std::string &auth) {
    auto j = Json::parse(auth);
    if (!j.is_object())
        Fail("Authentication JSON must be an object.");
    auto id = AccountId(j);
    if (id.empty() && j.contains("tokens") && j["tokens"].is_object())
        for (auto name : {"access_token", "id_token"}) {
            if (!j["tokens"].contains(name) || !j["tokens"][name].is_string())
                continue;
            auto token = j["tokens"][name].get<std::string>();
            auto a = token.find('.'), b = token.find('.', a == std::string::npos ? 0 : a + 1);
            if (a == std::string::npos || b == std::string::npos)
                continue;
            auto payload = token.substr(a + 1, b - a - 1);
            std::replace(payload.begin(), payload.end(), '-', '+');
            std::replace(payload.begin(), payload.end(), '_', '/');
            while (payload.size() % 4)
                payload += '=';
#ifdef _WIN32
            DWORD n = 0;
            if (CryptStringToBinaryA(payload.c_str(), DWORD(payload.size()), CRYPT_STRING_BASE64, nullptr, &n,
                                     nullptr, nullptr)) {
                std::string decoded(n, 0);
                if (CryptStringToBinaryA(payload.c_str(), DWORD(payload.size()), CRYPT_STRING_BASE64,
                                         reinterpret_cast<BYTE *>(decoded.data()), &n, nullptr, nullptr)) {
                    auto p = Json::parse(decoded, nullptr, false);
                    if (!p.is_discarded())
                        id = AccountId(p);
                }
            }
#else
            std::string decoded(payload.size(),'\0');
            int n=EVP_DecodeBlock(reinterpret_cast<unsigned char*>(decoded.data()),reinterpret_cast<const unsigned char*>(payload.data()),int(payload.size()));
            if(n>=0){size_t padding=0;for(size_t i=payload.size();i&&payload[i-1]=='=';--i)++padding;
                if(size_t(n)>=padding){decoded.resize(size_t(n)-padding);auto parsed=Json::parse(decoded,nullptr,false);if(!parsed.is_discarded())id=AccountId(parsed);}}
#endif
            if (!id.empty())
                break;
        }
    auto source = id.empty() ? auth : "codex-account:" + id;
#ifdef _WIN32
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    BYTE result[32];
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        Fail("SHA256 is unavailable.");
    bool ok = BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) >= 0 &&
              BCryptHashData(hash, reinterpret_cast<PUCHAR>(source.data()), ULONG(source.size()), 0) >= 0 &&
              BCryptFinishHash(hash, result, 32, 0) >= 0;
    if (hash)
        BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    if (!ok)
        Fail("Could not identify the account.");
#else
    unsigned char result[32];if(!SHA256(reinterpret_cast<const unsigned char*>(source.data()),source.size(),result))Fail("Could not identify the account.");
#endif
    std::string hex;
    const char *digits = "0123456789ABCDEF";
    for (auto c : result) {
        hex += digits[c >> 4];
        hex += digits[c & 15];
    }
    return hex;
}
#ifdef _WIN32
static std::string Crypt(const std::string &s, bool encrypt) {
    std::string entropy = "CodexAccountSwitcher/auth/v1";
    DATA_BLOB in{DWORD(s.size()), reinterpret_cast<BYTE *>(const_cast<char *>(s.data()))},
        ent{DWORD(entropy.size()), reinterpret_cast<BYTE *>(entropy.data())}, out{};
    bool ok = encrypt
                  ? CryptProtectData(&in, nullptr, &ent, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)
                  : CryptUnprotectData(&in, nullptr, &ent, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out);
    if (!ok)
        Fail("Windows could not unlock this profile. Use the same Windows user or "
             "sign in again.");
    std::string result(reinterpret_cast<char *>(out.pbData), out.cbData);
    platform::Erase(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return result;
}
std::string Protect(const std::string &s) {
    return Crypt(s, true);
}
std::string Unprotect(const std::string &s) {
    return Crypt(s, false);
}
#endif
Json Store::Metadata() const {
    auto path = root / "profiles.json";
    if (!std::filesystem::exists(path))
        return Json::array();
    auto j = Json::parse(Read(path));
    if (!j.is_array())
        Fail("The profile list is invalid.");
    std::set<std::string> seen;
    for (auto &p : j) {
        if (!p.is_object() || !p.contains("id") || !p["id"].is_string() ||
            !ValidId(p["id"].get<std::string>()) || !seen.insert(p["id"].get<std::string>()).second)
            Fail("The profile list contains an invalid ID.");
    }
    return j;
}
int Store::ImportLegacy() {
    if (std::filesystem::exists(root / "profiles.json"))
        return 0;
#ifdef _WIN32
    const auto local = UserHome() / L"AppData/Local";
    std::vector<Path> candidates = {local / L"CodexAccountSwitcher",
                                    local / L"Packages/OpenAI.Codex_2p2nqsd0c76g0/"
                                            L"LocalCache/Local/CodexAccountSwitcher"};
#else
    std::vector<Path> candidates;
#endif
    for (auto &candidate : candidates) {
        if (!std::filesystem::exists(candidate / "profiles.json"))
            continue;
        Store old(candidate);
        auto metadata = old.Metadata();
        if (metadata.empty())
            continue;
        std::vector<std::pair<Path, std::string>> blobs;
        for (auto &p : metadata) {
            auto id = p["id"].get<std::string>();
            blobs.emplace_back(root / "profiles" / (id + ".auth.dpapi"),
                               Read(candidate / "profiles" / (id + ".auth.dpapi")));
        }
        // Publish metadata last. Original encrypted files remain untouched.
        for (auto &[path, data] : blobs) {
            if (std::filesystem::exists(path)) {
                if (Read(path) != data)
                    Fail("Import conflict; existing encrypted profile was retained.");
            } else
                WriteAtomic(path, data);
        }
        WriteAtomic(root / "profiles.json", metadata.dump(2));
        return int(metadata.size());
    }
    return 0;
}
static double Remaining(const Json &p, const char *name) {
    if (!p.is_object() || !p.contains(name) || !p[name].is_object())
        return -1;
    auto &w = p[name];
    if (w.contains("remainingPercent") && w["remainingPercent"].is_number()) {
        double v = w["remainingPercent"].get<double>();
        if (std::isfinite(v))
            return std::clamp(v, 0., 100.);
    }
    return -1;
}
std::vector<Profile> Store::Load(bool activeBadge) const {
    auto metadata = Metadata();
    std::string active;
    if (activeBadge)
        try {
            if (std::filesystem::exists(ActiveAuth()))
                active = Fingerprint(Read(ActiveAuth()));
        } catch (...) {
        }
    std::vector<Profile> rows;
    for (auto &p : metadata) {
        Profile r;
        r.id = p["id"].get<std::string>();
        r.label = p.value("displayName", "Account");
        r.color = p.value("colorHex", "#4296FA");
        r.fingerprint = p.value("fingerprint", "");
        r.active = !active.empty() && active == r.fingerprint;
        r.available = std::filesystem::exists(root / "profiles" / (r.id + ".auth.dpapi"));
        if (p.contains("usage") && p["usage"].is_object()) {
            auto &u = p["usage"];
            if (u.contains("planType") && u["planType"].is_string())
                r.plan = u["planType"].get<std::string>();
            if (u.contains("checkedAt") && u["checkedAt"].is_string())
                r.checkedAt = u["checkedAt"].get<std::string>();
            r.weekly = Remaining(u, "weekly");
            r.shortTerm = Remaining(u, "shortTerm");
            auto reset = [&](const char *key) -> int64_t {
                if (!u.contains(key) || !u[key].is_object() || !u[key].contains("resetsAt"))
                    return 0;
                const auto &value = u[key]["resetsAt"];
                if (value.is_number_integer()) {
                    if (value.is_number_unsigned() && value.get<uint64_t>() > 253402300799ULL)
                        return 0;
                    auto seconds = value.get<int64_t>();
                    return seconds > 0 && seconds <= 253402300799LL ? seconds : 0;
                }
                int64_t seconds = 0;
                if (value.is_string() && ParseUtcTimestamp(value.get<std::string>(), seconds))
                    return seconds;
                return 0;
            };
            r.shortResetsAt = reset("shortTerm");
            r.weeklyResetsAt = reset("weekly");
        }
        rows.push_back(r);
    }
    return rows;
}
std::string Store::Auth(const std::string &id) const {
    if (!ValidId(id))
        Fail("Invalid profile.");
    auto j = Metadata();
    for (auto &p : j)
        if (p["id"] == id) {
            auto auth = Unprotect(Read(root / "profiles" / (id + ".auth.dpapi")));
            if (Fingerprint(auth) != p.value("fingerprint", ""))
                Fail("Profile identity mismatch.");
            return auth;
        }
    Fail("Profile not found.");
    return {};
}
std::string Store::Add(const std::string &label, const std::string &color, const std::string &auth) {
    auto name = Label(label), fp = Fingerprint(auth);
    auto parsed = Json::parse(auth);
    if (!parsed.contains("tokens") && !parsed.contains("OPENAI_API_KEY"))
        Fail("No authentication credentials found.");
    auto j = Metadata();
    for (auto &p : j)
        if (p.value("fingerprint", "") == fp)
            Fail("This account is already saved.");
    auto id = Guid();
    auto path = root / "profiles" / (id + ".auth.dpapi");
    WriteAtomic(path, Protect(auth));
    j.push_back({{"id", id},
                 {"displayName", name},
                 {"colorHex", Color(color)},
                 {"fingerprint", fp},
                 {"createdAt", Timestamp()}});
    try {
        WriteAtomic(root / "profiles.json", j.dump(2));
    } catch (...) {
        platform::RemoveFile(path);
        throw;
    }
    return id;
}
void Store::SaveCurrent(const std::string &label, const std::string &color) {
    auto auth = Read(ActiveAuth());
    try {
        Add(label, color, auth);
    } catch (...) {
        platform::Erase(auth.data(), auth.size());
        throw;
    }
    platform::Erase(auth.data(), auth.size());
}
void Store::Rename(const std::string &id, const std::string &label, const std::string &color) {
    auto j = Metadata();
    bool found = false;
    for (auto &p : j)
        if (p["id"] == id) {
            p["displayName"] = Label(label);
            p["colorHex"] = Color(color);
            found = true;
        }
    if (!found)
        Fail("Profile not found.");
    WriteAtomic(root / "profiles.json", j.dump(2));
}
void Store::Move(const std::string &id, const std::string &anchorId, bool after) {
    if (!ValidId(id) || !ValidId(anchorId))
        Fail("Invalid account order.");
    if (id == anchorId)
        return;
    auto metadata = Metadata();
    Json moving;
    Json rest = Json::array();
    bool anchorFound = false;
    for (const auto &p : metadata) {
        if (p["id"] == id)
            moving = p;
        else
            rest.push_back(p);
        if (p["id"] == anchorId)
            anchorFound = true;
    }
    if (moving.is_null() || !anchorFound)
        Fail("An account no longer exists. Reload the list.");
    Json reordered = Json::array();
    for (const auto &p : rest) {
        if (p["id"] == anchorId && !after)
            reordered.push_back(moving);
        reordered.push_back(p);
        if (p["id"] == anchorId && after)
            reordered.push_back(moving);
    }
    if (reordered != metadata)
        WriteAtomic(root / "profiles.json", reordered.dump(2));
}
void Store::Remove(const std::string &id) {
    if (!ValidId(id))
        Fail("Invalid profile.");
    auto j = Metadata();
    Json keep = Json::array();
    bool found = false;
    for (auto &p : j) {
        if (p["id"] == id)
            found = true;
        else
            keep.push_back(p);
    }
    if (!found)
        Fail("Profile not found.");
    WriteAtomic(root / "profiles.json", keep.dump(2));
    auto file = root / "profiles" / (id + ".auth.dpapi");
    if (std::filesystem::exists(file) && !platform::RemoveFile(file))
        Fail("Removed from list, but encrypted file could not be deleted.");
}
void Store::UpdateUsage(const std::string &id, const Json &usage, const std::string &auth) {
    auto j = Metadata();
    for (auto &p : j)
        if (p["id"] == id) {
            if (!auth.empty()) {
                if (Fingerprint(auth) != p.value("fingerprint", ""))
                    Fail("Refreshed account identity mismatch.");
                WriteAtomic(root / "profiles" / (id + ".auth.dpapi"), Protect(auth));
            }
            p["usage"] = usage;
            WriteAtomic(root / "profiles.json", j.dump(2));
            return;
        }
    Fail("Profile not found.");
}
#ifdef _WIN32
void LaunchCodex() {
    auto result = ShellExecuteW(nullptr, L"open", L"shell:AppsFolder\\OpenAI.Codex_2p2nqsd0c76g0!App",
                                nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32)
        Fail("Open Codex manually from the Start menu.");
}
#endif
void Store::Activate(const std::string &id, const Path &activePath, bool checkProcesses) {
    auto target = Auth(id);
    try {
        if (checkProcesses && CodexIsRunning())
            Fail("Codex is still running. The account was not changed.");
        auto j = Metadata();
        auto expected = Fingerprint(target);
        bool had = std::filesystem::exists(activePath);
        if (had) {
            auto departing = Read(activePath);
            auto fp = Fingerprint(departing);
            for (auto &p : j)
                if (p.value("fingerprint", "") == fp)
                    WriteAtomic(root / "profiles" / (p["id"].get<std::string>() + ".auth.dpapi"),
                                Protect(departing));
            platform::Erase(departing.data(), departing.size());
        }
        std::filesystem::create_directories(activePath.parent_path());
        auto temp = activePath;
        temp += ".switching-" + Guid();
        auto backup = activePath;
        backup += ".backup-" + Guid();
        WriteAtomic(temp, target);
        bool changed = false;
        try {
            if (checkProcesses && CodexIsRunning())
                Fail("Codex restarted. Close it and retry.");
#ifdef _WIN32
            BOOL ok =
                had ? ReplaceFileW(activePath.c_str(), temp.c_str(), backup.c_str(), 0, nullptr, nullptr)
                    : MoveFileExW(temp.c_str(), activePath.c_str(), MOVEFILE_WRITE_THROUGH);
#else
            if(had)WriteAtomic(backup,Read(activePath));
            WriteAtomic(activePath,target);
            platform::RemoveFile(temp);
            bool ok=true;
#endif
            if (!ok)
                Fail("Could not replace active authentication. Current account "
                     "retained.");
            changed = true;
            if (Fingerprint(Read(activePath)) != expected)
                Fail("Account verification failed after replacement.");
        } catch (...) {
            if (changed) {
                if (had) {
#ifdef _WIN32
                    if (!CopyFileW(backup.c_str(), activePath.c_str(), FALSE))
                        Fail("Rollback failed. The original authentication backup is next "
                             "to auth.json.");
#else
                    WriteAtomic(activePath,Read(backup));
#endif
                } else
                    platform::RemoveFile(activePath);
            }
            platform::RemoveFile(temp);
            throw;
        }
    } catch (...) {
        platform::Erase(target.data(), target.size());
        throw;
    }
    platform::Erase(target.data(), target.size());
}
bool SelfTest() {
    auto base = std::filesystem::temp_directory_path() / L"CodexImGuiTests" / Guid();
    Store store(base / "store");
    bool ok = false;
    try {
        std::string a = R"({"tokens":{"account_id":"test-one","access_token":"synthetic"}})",
                    b = R"({"tokens":{"account_id":"test-two","access_token":"synthetic-2"}})";
        store.Add("First", "#4296FA", a);
        store.Add("Second", "#AA66FF", b);
        auto rows = store.Load(false);
        ok = rows.size() == 2 && store.Auth(rows[0].id) == a;
        int64_t expectedWeekly = 0;
        ParseUtcTimestamp("2026-09-08T15:00:00+03:00", expectedWeekly);
        store.UpdateUsage(
            rows[0].id,
            Json{{"shortTerm", {{"remainingPercent", 84}, {"resetsAt", 1788613200LL}}},
                 {"weekly", {{"remainingPercent", 72}, {"resetsAt", "2026-09-08T15:00:00+03:00"}}}},
            "");
        auto limits = Store(store.root).Load(false);
        ok = ok && limits[0].shortResetsAt == 1788613200LL && limits[0].weeklyResetsAt == expectedWeekly &&
             limits[0].shortResetsAt != limits[0].weeklyResetsAt;
        store.UpdateUsage(rows[1].id,
                          Json{{"shortTerm", {{"remainingPercent", 40}, {"resetsAt", nullptr}}},
                               {"weekly", {{"remainingPercent", 50}, {"resetsAt", "invalid"}}}},
                          "");
        auto missing = store.Load(false);
        ok = ok && missing[1].shortResetsAt == 0 && missing[1].weeklyResetsAt == 0;
        auto active = base / "auth.json";
        WriteAtomic(active, a);
        store.Activate(rows[1].id, active, false);
        ok = ok && Read(active) == b;
        bool duplicate = false;
        try {
            store.Add("Duplicate", "#FFFFFF", a);
        } catch (...) {
            duplicate = true;
        }
        ok = ok && duplicate;
        auto encryptedBefore = Read(store.root / "profiles" / (rows[0].id + ".auth.dpapi"));
        store.Move(rows[1].id, rows[0].id, false);
        ok = ok && Store(store.root).Load(false)[0].id == rows[1].id;
        store.Move(rows[1].id, rows[0].id, true);
        ok = ok && Store(store.root).Load(false)[0].id == rows[0].id;
        auto orderBefore = store.Metadata();
        bool invalidMove = false;
        try {
            store.Move(rows[0].id, Guid(), false);
        } catch (...) {
            invalidMove = true;
        }
        ok = ok && invalidMove && orderBefore == store.Metadata() &&
             encryptedBefore == Read(store.root / "profiles" / (rows[0].id + ".auth.dpapi"));
        store.Rename(rows[0].id, "Renamed", "#FFFFFF");
        ok = ok && store.Load(false)[0].label == "Renamed";
        store.Remove(rows[0].id);
        ok = ok && store.Load(false).size() == 1;
        auto malicious = store.Metadata();
        malicious[0]["id"] = "../escape";
        WriteAtomic(store.root / "profiles.json", malicious.dump());
        bool blocked = false;
        try {
            store.Metadata();
        } catch (...) {
            blocked = true;
        }
        ok = ok && blocked;
    } catch (...) {
        ok = false;
    }
    std::error_code ec;
    std::filesystem::remove_all(base, ec);
    return ok;
}
} // namespace accounts
