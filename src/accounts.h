#pragma once
#include "nlohmann/json.hpp"
#include <atomic>
#include <filesystem>
#include <string>
#include <vector>
namespace accounts {
struct OperationCanceled : std::runtime_error {
    OperationCanceled() : std::runtime_error("Operation canceled") {}
};
using Json = nlohmann::json;
using Path = std::filesystem::path;
struct Profile {
    std::string id, label, color, fingerprint, plan, checkedAt;
    double weekly = -1, shortTerm = -1;
    bool active = false, available = false;
    int64_t shortResetsAt = 0, weeklyResetsAt = 0;
};
std::wstring Wide(const std::string &text);
std::string Utf8(const std::wstring &text);
std::string Read(const Path &path);
void WriteAtomic(const Path &path, const std::string &content);
std::string Guid();
std::string Timestamp();
std::string Fingerprint(const std::string &auth);
std::string Protect(const std::string &auth);
std::string Unprotect(const std::string &auth);
Path UserHome();
Path ActiveAuth();
Path FindCli();
bool CodexIsRunning();
void LaunchCodex();
struct Store {
    explicit Store(Path path) : root(std::move(path)) {}
    Path root;
    int ImportLegacy();
    Json Metadata() const;
    std::vector<Profile> Load(bool activeBadge = true) const;
    std::string Auth(const std::string &id) const;
    void SaveCurrent(const std::string &label, const std::string &color);
    std::string Add(const std::string &label, const std::string &color, const std::string &auth);
    void Rename(const std::string &id, const std::string &label, const std::string &color);
    void Remove(const std::string &id);
    void Move(const std::string &id, const std::string &anchorId, bool after);
    void UpdateUsage(const std::string &id, const Json &result, const std::string &updatedAuth);
    void Activate(const std::string &id, const Path &activePath, bool checkProcesses = true);
};
std::string Login(Store store, const std::string &label, const std::string &color, std::atomic_bool &cancel);
void Refresh(Store store, const std::string &id, std::atomic_bool &cancel);
bool Probe(std::atomic_bool &cancel);
bool SelfTest();
bool ProtocolSelfTest();
int FakeServer();
std::string SwitchInHelper(const std::string &id);
int SwitchHelper(const std::string &id, bool dryRun = false);
bool SwitchSelfTest();
bool SwitchHelperSelfTest();
} // namespace accounts
