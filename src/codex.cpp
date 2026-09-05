#include <windows.h>
#include "accounts.h"
#include "platform.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <memory>
#include <sddl.h>
#include <shellapi.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <softpub.h>
#include <thread>
#include <wincrypt.h>
namespace accounts {
struct Handle {
    HANDLE h = nullptr;
    Handle() = default;
    explicit Handle(HANDLE value) : h(value) {}
    ~Handle() {
        if (h && h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
    }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    void reset() {
        if (h && h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
        h = nullptr;
    }
};
static void Require(bool ok, const char *msg) {
    if (!ok)
        throw std::runtime_error(msg);
}
static bool SignedOpenAI(const Path &path) {
    WINTRUST_FILE_INFO file{sizeof(file)};
    file.pcwszFilePath = path.c_str();
    WINTRUST_DATA data{sizeof(data)};
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_NONE;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &file;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    LONG status = WinVerifyTrust(nullptr, &action, &data);
    bool trusted = false;
    if (status == ERROR_SUCCESS) {
        auto *state = WTHelperProvDataFromStateData(data.hWVTStateData);
        auto *signer = state ? WTHelperGetProvSignerFromChain(state, 0, FALSE, 0) : nullptr;
        if (signer && signer->csCertChain) {
            wchar_t name[256]{};
            CertGetNameStringW(signer->pasCertChain[0].pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, name,
                               256);
            trusted = std::wstring(name) == L"OpenAI OpCo, LLC";
        }
    }
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &action, &data);
    return trusted;
}
Path FindCli() {
    auto local = UserHome() / L"AppData/Local";
    std::vector<Path> paths;
    auto versioned = local / L"OpenAI/Codex/bin";
    std::error_code ec;
    if (std::filesystem::is_directory(versioned, ec))
        for (auto &entry : std::filesystem::directory_iterator(versioned)) {
            auto p = entry.path() / L"codex.exe";
            if (std::filesystem::is_regular_file(p, ec))
                paths.push_back(p);
        }
    std::sort(paths.begin(), paths.end(), [](auto &a, auto &b) {
        return std::filesystem::last_write_time(a) > std::filesystem::last_write_time(b);
    });
    paths.push_back(local / L"Programs/OpenAI/Codex/bin/codex.exe");
    wchar_t overridePath[32768];DWORD overrideLength=GetEnvironmentVariableW(L"CODEX_CLI_PATH",overridePath,32768);
    if(overrideLength>0&&overrideLength<32768&&Path(overridePath).is_absolute())paths.insert(paths.begin(),overridePath);
    wchar_t discovered[32768];DWORD discoveredLength=SearchPathW(nullptr,L"codex.exe",nullptr,32768,discovered,nullptr);
    if(discoveredLength>0&&discoveredLength<32768)paths.push_back(discovered);
    for (auto &path : paths)
        if (std::filesystem::is_regular_file(path, ec) && SignedOpenAI(path))
            return path;
    throw std::runtime_error("A signed OpenAI Codex CLI was not found in the user installation.");
}
static void PrivateDirectory(const Path &path) {
    Handle token;
    Require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.h) != 0,
            "Could not secure temporary login data.");
    DWORD n = 0;
    GetTokenInformation(token.h, TokenUser, nullptr, 0, &n);
    std::vector<BYTE> info(n);
    Require(GetTokenInformation(token.h, TokenUser, info.data(), n, &n) != 0,
            "Could not read Windows user identity.");
    LPWSTR sid = nullptr;
    Require(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(info.data())->User.Sid, &sid) != 0,
            "Could not read Windows user identity.");
    std::wstring acl = L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;" + std::wstring(sid) + L")";
    LocalFree(sid);
    PSECURITY_DESCRIPTOR desc = nullptr;
    Require(ConvertStringSecurityDescriptorToSecurityDescriptorW(acl.c_str(), SDDL_REVISION_1, &desc,
                                                                 nullptr) != 0,
            "Could not secure login data.");
    SECURITY_ATTRIBUTES sa{sizeof(sa), desc, FALSE};
    bool created = CreateDirectoryW(path.c_str(), &sa) != 0;
    LocalFree(desc);
    Require(created, "Could not create a private login directory.");
}
class Session {
    Handle job, process, input, output;
    Path home;
    std::string buffer;
    std::atomic_bool &canceled;
    int nextId = 1;
    std::vector<Json> notifications;
    void Check() {
        if (canceled.load())
            throw OperationCanceled();
    }
    void Send(const Json &j) {
        Check();
        auto s = j.dump() + "\n";
        DWORD written = 0;
        Require(WriteFile(input.h, s.data(), DWORD(s.size()), &written, nullptr) && written == s.size(),
                "Codex App Server communication failed.");
    }
    Json ReadMessage(std::chrono::steady_clock::time_point deadline) {
        while (true) {
            Check();
            if (std::chrono::steady_clock::now() > deadline)
                throw std::runtime_error("Codex request timed out. You can try again.");
            auto end = buffer.find('\n');
            if (end != std::string::npos) {
                auto line = buffer.substr(0, end);
                buffer.erase(0, end + 1);
                auto j = Json::parse(line, nullptr, false);
                if (!j.is_discarded())
                    return j;
                continue;
            }
            DWORD bytes = 0;
            Require(PeekNamedPipe(output.h, nullptr, 0, nullptr, &bytes, nullptr) != 0,
                    "Codex App Server stopped.");
            if (bytes) {
                char data[4096];
                DWORD n = 0;
                Require(ReadFile(output.h, data, std::min<DWORD>(bytes, sizeof(data)), &n, nullptr) != 0,
                        "Codex App Server stopped.");
                buffer.append(data, n);
                Require(buffer.size() < 4 * 1024 * 1024, "Codex response exceeded the size limit.");
            } else {
                Require(WaitForSingleObject(process.h, 0) == WAIT_TIMEOUT, "Codex App Server stopped.");
                Sleep(20);
            }
        }
    }

  public:
    Session(Path base, std::atomic_bool &flag, const std::string &auth, bool fake = false) : canceled(flag) {
        Path cli;
        if (fake) {
            wchar_t path[32768];
            GetModuleFileNameW(nullptr, path, 32768);
            cli = path;
        } else
            cli = FindCli();
        Check();
        std::filesystem::create_directories(base);
        home = base / Wide(Guid());
        PrivateDirectory(home);
        try {
            WriteAtomic(home / L"config.toml", "cli_auth_credentials_store = \"file\"\n");
            if (!auth.empty())
                WriteAtomic(home / L"auth.json", auth);
            SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
            Handle childRead, childWrite;
            Require(CreatePipe(&childRead.h, &input.h, &sa, 0) != 0 &&
                        CreatePipe(&output.h, &childWrite.h, &sa, 0) != 0,
                    "Could not create Codex communication pipes.");
            SetHandleInformation(input.h, HANDLE_FLAG_INHERIT, 0);
            SetHandleInformation(output.h, HANDLE_FLAG_INHERIT, 0);
            Handle error(CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                                     OPEN_EXISTING, 0, nullptr));
            auto env = GetEnvironmentStringsW();
            std::vector<wchar_t> environment;
            for (auto ptr = env; *ptr; ptr += wcslen(ptr) + 1) {
                std::wstring value(ptr);
                if (_wcsnicmp(value.c_str(), L"CODEX_HOME=", 11) != 0)
                    environment.insert(environment.end(), ptr, ptr + wcslen(ptr) + 1);
            }
            FreeEnvironmentStringsW(env);
            std::wstring value = L"CODEX_HOME=" + home.wstring();
            environment.insert(environment.end(), value.begin(), value.end());
            environment.push_back(0);
            environment.push_back(0);
            STARTUPINFOEXW start{};
            start.StartupInfo.cb = sizeof(start);
            start.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
            start.StartupInfo.hStdInput = childRead.h;
            start.StartupInfo.hStdOutput = childWrite.h;
            start.StartupInfo.hStdError = error.h;
            SIZE_T size = 0;
            InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
            std::vector<BYTE> attrs(size);
            start.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrs.data());
            Require(InitializeProcThreadAttributeList(start.lpAttributeList, 1, 0, &size) != 0,
                    "Could not start the isolated process.");
            HANDLE handles[] = {childRead.h, childWrite.h, error.h};
            bool set = UpdateProcThreadAttribute(start.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                                 handles, sizeof(handles), nullptr, nullptr) != 0;
            PROCESS_INFORMATION pi{};
            std::wstring cmd = L"\"" + cli.wstring() + (fake ? L"\" --fake-server" : L"\" app-server");
            bool created = set && CreateProcessW(cli.c_str(), cmd.data(), nullptr, nullptr, TRUE,
                                                 CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT |
                                                     CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
                                                 environment.data(), home.c_str(), &start.StartupInfo, &pi);
            DeleteProcThreadAttributeList(start.lpAttributeList);
            Require(created, "Could not start the signed Codex CLI.");
            process.h = pi.hProcess;
            Handle thread(pi.hThread);
            job.h = CreateJobObjectW(nullptr, nullptr);
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};
            limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (!job.h ||
                !SetInformationJobObject(job.h, JobObjectExtendedLimitInformation, &limit, sizeof(limit)) ||
                !AssignProcessToJobObject(job.h, process.h)) {
                TerminateProcess(process.h, 1);
                Require(false, "Could not isolate the Codex process.");
            }
            Require(ResumeThread(thread.h) != DWORD(-1), "Could not resume Codex.");
            childRead.reset();
            childWrite.reset();
            Request(
                "initialize",
                {{"clientInfo",
                  {{"name", "codex_switcher"}, {"title", "Codex Switcher"}, {"version", "0.2.0"}}}});
            Send({{"method", "initialized"}, {"params", Json::object()}});
        } catch (...) {
            job.reset();
            if (process.h)
                WaitForSingleObject(process.h, 3000);
            std::error_code ec;
            std::filesystem::remove_all(home, ec);
            throw;
        }
    }
    ~Session() {
        input.reset();
        job.reset();
        if (process.h)
            WaitForSingleObject(process.h, 3000);
        output.reset();
        process.reset();
        std::error_code ec;
        std::filesystem::remove_all(home, ec);
    }
    Json Request(const std::string &method, const Json &params = Json::object()) {
        int id = nextId++;
        Send({{"id", id}, {"method", method}, {"params", params}});
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(40);
        while (true) {
            auto j = ReadMessage(deadline);
            if (j.contains("id") && j["id"] == id) {
                Require(!j.contains("error"), "Codex rejected the request. Sign in again or retry later.");
                return j.at("result");
            }
            if (j.contains("method"))
                notifications.push_back(j);
        }
    }
    Json WaitLogin() {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(10);
        while (true) {
            for (auto it = notifications.begin(); it != notifications.end(); ++it)
                if (it->value("method", "") == "account/login/completed") {
                    auto result = it->at("params");
                    notifications.erase(it);
                    return result;
                }
            auto j = ReadMessage(deadline);
            if (j.value("method", "") == "account/login/completed")
                return j.at("params");
        }
    }
    std::string Auth() {
        auto p = home / L"auth.json";
        return std::filesystem::exists(p) ? Read(p) : "";
    }
};
#include "codex_protocol.inc"
int FakeServer() {
    std::string line;
    char ch;
    DWORD read = 0;
    while (ReadFile(GetStdHandle(STD_INPUT_HANDLE), &ch, 1, &read, nullptr) && read) {
        if (ch != '\n') {
            line += ch;
            continue;
        }
        auto request = Json::parse(line, nullptr, false);
        line.clear();
        if (!request.is_object() || !request.contains("id"))
            continue;
        Json result = Json::object();
        if (request.value("method", "") == "account/read")
            result["account"] = nullptr;
        std::string response = Json({{"id", request["id"]}, {"result", result}}).dump() + "\n";
        DWORD written = 0;
        if (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), response.data(), DWORD(response.size()), &written,
                       nullptr))
            return 1;
    }
    return 0;
}
bool ProtocolSelfTest() {
    auto base = std::filesystem::temp_directory_path() / L"CodexImGuiProtocolTests" / Wide(Guid());
    std::atomic_bool cancel = false;
    bool ok = false;
    try {
        {
            Session session(base, cancel, "", true);
            auto reply = session.Request("account/read");
            ok = reply.contains("account") && reply["account"].is_null();
            auto start = std::chrono::steady_clock::now();
            std::thread trigger([&] {
                Sleep(150);
                cancel = true;
            });
            bool canceled = false;
            try {
                session.WaitLogin();
            } catch (const std::runtime_error &) {
                canceled = cancel.load();
            }
            trigger.join();
            ok = ok && canceled && std::chrono::steady_clock::now() - start < std::chrono::seconds(2);
        }
        ok = ok && std::filesystem::is_empty(base);
    } catch (...) {
        ok = false;
    }
    std::error_code ec;
    std::filesystem::remove_all(base, ec);
    return ok;
}
} // namespace accounts
