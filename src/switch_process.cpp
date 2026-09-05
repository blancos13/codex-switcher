#include <windows.h>
#include <appmodel.h>
#include <tlhelp32.h>
#include "accounts.h"
#include "platform.h"
#include <algorithm>
#include <functional>
#include <memory>
#include <set>
#include <map>

namespace accounts {
namespace {
struct Process {
    HANDLE handle = nullptr;
    DWORD id = 0;
    explicit Process(DWORD pid) : id(pid) {
        handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
        if (!handle)
            throw std::runtime_error("A running process could not be identified. Switch canceled.");
    }
    ~Process() {
        if (handle)
            CloseHandle(handle);
    }
    bool Alive() const {
        return WaitForSingleObject(handle, 0) == WAIT_TIMEOUT;
    }
    std::wstring Image() const {
        wchar_t path[32768];
        DWORD n = 32768;
        if (!QueryFullProcessImageNameW(handle, 0, path, &n))
            throw std::runtime_error("Could not identify a running Codex process.");
        return std::wstring(path, n);
    }
    std::wstring Family() const {
        wchar_t name[512];
        UINT32 n = 512;
        LONG code = GetPackageFamilyName(handle, &n, name);
        if (code == APPMODEL_ERROR_NO_PACKAGE)
            return {};
        if (code != ERROR_SUCCESS)
            throw std::runtime_error("Could not verify the Codex package identity.");
        return name;
    }
};
using Proc = std::shared_ptr<Process>;
struct Entry {
    DWORD id, parent;
    std::wstring name;
};
std::vector<Entry> Entries() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        throw std::runtime_error("Could not enumerate running applications.");
    PROCESSENTRY32W e{sizeof(e)};
    std::vector<Entry> entries;
    if (Process32FirstW(snap, &e))
        do {
            entries.push_back({e.th32ProcessID, e.th32ParentProcessID, e.szExeFile});
        } while (Process32NextW(snap, &e));
    CloseHandle(snap);
    return entries;
}
bool IsCodexGui(const std::wstring &image, const std::wstring &family) {
    return family == L"OpenAI.Codex_2p2nqsd0c76g0" &&
           _wcsicmp(Path(image).filename().c_str(), L"ChatGPT.exe") == 0;
}
std::vector<Proc> FindDesktop() {
    std::vector<Proc> found;
    for (auto &e : Entries()) {
        if (_wcsicmp(e.name.c_str(), L"ChatGPT.exe") != 0)
            continue;
        Proc p;
        try {
            p = std::make_shared<Process>(e.id);
        } catch (...) {
            if (GetLastError() == ERROR_INVALID_PARAMETER)
                continue;
            throw;
        }
        if (!p->Alive())
            continue;
        try {
            if (IsCodexGui(p->Image(), p->Family()))
                found.push_back(p);
        } catch (...) {
            if (p->Alive())
                throw;
        }
    }
    return found;
}
BOOL CALLBACK CloseWindow(HWND hwnd, LPARAM parameter) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == DWORD(parameter) && IsWindowVisible(hwnd))
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
    return TRUE;
}
ULONGLONG Created(HANDLE h) {
    FILETIME c, e, k, u;
    if (!GetProcessTimes(h, &c, &e, &k, &u))
        throw std::runtime_error("Could not verify process creation time.");
    return (ULONGLONG(c.dwHighDateTime) << 32) | c.dwLowDateTime;
}
void StopTree(const Proc &root) {
    auto entries = Entries();
    std::map<DWORD, Proc> owned;
    owned[root->id] = root;
    bool added = true;
    while (added) {
        added = false;
        for (auto &e : entries) {
            if (owned.count(e.id) || !owned.count(e.parent))
                continue;
            try {
                auto p = std::make_shared<Process>(e.id);
                if (p->Alive() && Created(p->handle) >= Created(owned.at(e.parent)->handle)) {
                    owned[e.id] = p;
                    added = true;
                }
            } catch (...) {
                if (GetLastError() != ERROR_INVALID_PARAMETER)
                    throw;
            }
        }
    }
    // Hold query handles while opening termination handles so IDs cannot be silently recycled.
    std::vector<HANDLE> terminateHandles;
    try {
        for (auto &[id, p] : owned) {
            if (!p->Alive() || id == GetCurrentProcessId())
                continue;
            HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, id);
            if (!h)
                throw std::runtime_error("Codex could not be closed. Authentication was not changed.");
            terminateHandles.push_back(h);
        }
        for (auto h : terminateHandles)
            if (WaitForSingleObject(h, 0) == WAIT_TIMEOUT && !TerminateProcess(h, 0))
                throw std::runtime_error("Codex could not be stopped.");
        for (auto h : terminateHandles) {
            if (WaitForSingleObject(h, 5000) != WAIT_OBJECT_0)
                throw std::runtime_error("Codex shutdown timed out.");
        }
    } catch (...) {
        for (auto h : terminateHandles)
            CloseHandle(h);
        throw;
    }
    for (auto h : terminateHandles)
        CloseHandle(h);
}
template <class Scan, class Stop, class Wait>
void AwaitQuiescence(Scan scan, Stop stop, Wait wait, int maxScans) {
    int empty = 0;
    for (int i = 0; i < maxScans; i++) {
        auto current = scan();
        if (current.empty()) {
            if (++empty == 3)
                return;
        } else {
            empty = 0;
            for (auto &p : current)
                stop(p);
        }
        wait();
    }
    throw std::runtime_error("Codex kept restarting. Authentication was not changed.");
}
void CloseDesktop() {
    auto initial = FindDesktop();
    for (auto &p : initial)
        EnumWindows(CloseWindow, LPARAM(p->id));
    ULONGLONG deadline = GetTickCount64() + 10000;
    while (GetTickCount64() < deadline &&
           std::any_of(initial.begin(), initial.end(), [](auto &p) { return p->Alive(); }))
        Sleep(100);
    // A GUI root may exit before one of its children; collect the tree in either case.
    for (auto &p : initial)
        StopTree(p);
    AwaitQuiescence(FindDesktop, StopTree, [] { Sleep(200); }, 30);
}
void Relaunch() {
    LaunchCodex();
    ULONGLONG until = GetTickCount64() + 15000;
    while (GetTickCount64() < until) {
        if (!FindDesktop().empty())
            return;
        Sleep(200);
    }
    throw std::runtime_error("The account was switched, but Codex did not reopen. Open it from Start.");
}
template <class Validate, class Close, class Replace, class Launch>
void SwitchSequence(Validate validate, Close close, Replace replace, Launch launch) {
    validate();
    try {
        close();
        close();
        replace();
    } catch (...) {
        auto failure = std::current_exception();
        try {
            launch();
        } catch (...) {
        }
        std::rethrow_exception(failure);
    }
    launch();
}
Path Executable() {
    wchar_t path[32768];
    GetModuleFileNameW(nullptr, path, 32768);
    return path;
}
void ValidProfileId(const std::string &id) {
    if (id.size() != 32 ||
        !std::all_of(id.begin(), id.end(), [](unsigned char c) { return isxdigit(c) != 0; }))
        throw std::runtime_error("Invalid account selection.");
}
DWORD StartHelper(const std::string &id, bool dryRun) {
    auto exe = Executable();
    std::wstring command =
        L"\"" + exe.wstring() + L"\" " + (dryRun ? L"--switch-helper-test " : L"--switch-helper ") + Wide(id);
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION pi{};
    BOOL inJob = FALSE;
    IsProcessInJob(GetCurrentProcess(), nullptr, &inJob);
    DWORD flags = CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP | (inJob ? CREATE_BREAKAWAY_FROM_JOB : 0);
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, flags, nullptr,
                        exe.parent_path().c_str(), &startup, &pi))
        throw std::runtime_error(
            "Could not start an independent switch worker. Open this app from Explorer and retry.");
    CloseHandle(pi.hThread);
    DWORD exitCode = 1;
    DWORD waited = WaitForSingleObject(pi.hProcess, 90000);
    if (waited == WAIT_OBJECT_0)
        GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    if (waited != WAIT_OBJECT_0)
        throw std::runtime_error("Switch is still finishing. Wait for Codex to reopen before trying again.");
    return exitCode;
}
} // namespace
int SwitchHelper(const std::string &id, bool dryRun) {
    if (dryRun)
        return 0;
    auto root = platform::DataDirectory(Executable().parent_path()) / "accounts";
    std::string message;
    bool failed = false;
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\CodexSwitcher.SwitchWorker");
    if (!mutex)
        return 2;
    DWORD lock = WaitForSingleObject(mutex, 0);
    if (lock != WAIT_OBJECT_0 && lock != WAIT_ABANDONED) {
        CloseHandle(mutex);
        return 3;
    }
    try {
        ValidProfileId(id);
        Store store(root);
        SwitchSequence(
            [&] {
                auto auth = store.Auth(id);
                SecureZeroMemory(auth.data(), auth.size());
            },
            CloseDesktop, [&] { store.Activate(id, ActiveAuth(), true); }, Relaunch);
        message = "Account switched. Codex reopened.";
    } catch (const Json::exception &) {
        message = "Invalid local account data. Switch failed.";
        failed = true;
    } catch (const std::exception &e) {
        message = e.what();
        failed = true;
    }
    try {
        WriteAtomic(root / L"switch-result.json",
                    Json({{"id", id}, {"message", message}, {"error", failed}, {"at", Timestamp()}}).dump());
    } catch (...) {
    }
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return failed ? 1 : 0;
}
std::string SwitchInHelper(const std::string &id) {
    ValidProfileId(id);
    DWORD code = StartHelper(id, false);
    if (code == 3)
        throw std::runtime_error("Another account switch is already in progress.");
    auto result = Json::parse(Read(platform::DataDirectory(Executable().parent_path()) / "accounts/switch-result.json"));
    if (result.value("id", "") != id)
        throw std::runtime_error("Could not verify the switch result.");
    if (code != 0 || result.value("error", true))
        throw std::runtime_error(result.value("message", "Switch failed."));
    return result.value("message", "Account switched.");
}
bool SwitchHelperSelfTest() {
    try {
        return StartHelper("test", true) == 0;
    } catch (...) {
        return false;
    }
}
bool CodexIsRunning() {
    return !FindDesktop().empty();
}
bool SwitchSelfTest() {
    bool ok = IsCodexGui(L"C:\\Program Files\\WindowsApps\\OpenAI.Codex_test\\app\\ChatGPT.exe",
                         L"OpenAI.Codex_2p2nqsd0c76g0") &&
              !IsCodexGui(L"C:\\Other\\ChatGPT.exe", L"OpenAI.ChatGPT_other") &&
              !IsCodexGui(L"C:\\Other\\codex.exe", L"OpenAI.Codex_2p2nqsd0c76g0");
    std::string trace;
    SwitchSequence([&] { trace += 'V'; }, [&] { trace += 'C'; }, [&] { trace += 'R'; },
                   [&] { trace += 'L'; });
    ok = ok && trace == "VCCRL";
    trace.clear();
    try {
        SwitchSequence([&] { trace += 'V'; }, [&] { trace += 'C'; },
                       [&] {
                           trace += 'R';
                           throw std::runtime_error("test");
                       },
                       [&] { trace += 'L'; });
        ok = false;
    } catch (...) {
        ok = ok && trace == "VCCRL";
    }
    trace.clear();
    try {
        SwitchSequence([&] { throw std::runtime_error("invalid profile"); }, [&] { trace += 'C'; },
                       [&] { trace += 'R'; }, [&] { trace += 'L'; });
        ok = false;
    } catch (...) {
        ok = ok && trace.empty();
    }
    int scans = 0, kills = 0;
    AwaitQuiescence(
        [&] {
            ++scans;
            return scans == 2 ? std::vector<int>{1} : std::vector<int>{};
        },
        [&](int) { ++kills; }, [] {}, 10);
    ok = ok && scans == 5 && kills == 1;
    try {
        AwaitQuiescence([] { return std::vector<int>{1}; }, [](int) {}, [] {}, 4);
        ok = false;
    } catch (...) {
    }
    return ok;
}
} // namespace accounts
