#pragma once
#include "accounts.h"
#include "imgui.h"
#include "refresh_schedule.h"
#include <array>
#include <functional>
#include <future>
#include <map>
struct OperationResult {
    std::string message;
    std::string addedAccountId;
    std::string installationInfo;
    bool error = false, canceled = false;
    std::map<std::string, std::string> usageErrors;
    OperationResult(std::string text) : message(std::move(text)) {}
    OperationResult(const char *text) : message(text) {}
};
struct Settings {
    int theme = 0, font = 2, density = 1;
    float scale = 1, rounding = 0;
};
bool SaveSettings(const std::filesystem::path &, const Settings &, std::string &);
bool LoadSettings(const std::filesystem::path &, Settings &, std::string &);
bool SettingsSelfTest();
class Studio {
  public:
    Studio(std::filesystem::path root, bool preview = false);
    ~Studio();
    void InitFonts();
    void Draw();
    void Save();
    void Tick();
    static bool UiSelfTest();
    Settings settings;
    bool exitRequested = false;
    int forcedTab = -1;
    std::string captureState;
    std::string status = "Ready";

  private:
    std::filesystem::path root_;
    accounts::Store store_;
    std::array<ImFont *, 3> fonts_{};
    std::vector<accounts::Profile> profiles_;
    std::future<OperationResult> task_;
    std::atomic_bool cancel_{false};
    bool preview_ = false, busy_ = false, login_ = false, error_ = false, dirty_ = false, openAdd_ = false,
         closeAdd_ = false, refreshingAll_ = false, addVisible_ = false, switching_ = false;
    int requestedTab_ = -1;
    float color_[3] = {.26f, .59f, .98f};
    char label_[161]{}, search_[128]{}, rename_[161]{};
    std::string pendingId_, pendingName_, pendingColor_, appearanceStatus_;
    std::string checkingUsageId_;
    std::string installationInfo_;
    std::function<void(accounts::Store, const std::string &, std::atomic_bool &)> refreshUsage_ =
        accounts::Refresh;
    std::map<std::string, std::string> usageErrors_;
    std::atomic_int refreshCompleted_{0};
    int refreshTotal_ = 0;
    RefreshSchedule refreshSchedule_;
    bool modalOpen_ = false;
    void Reload();
    void Start(std::function<OperationResult()> action, bool login = false);
    void RefreshAll();
    void CheckNewAccountUsage(const std::string &id);
    void ApplyTheme();
    void Accounts();
    void AddAccount();
    void Appearance();
    void Storage();
    void Dialogs();
    std::string ColorHex() const;
};
