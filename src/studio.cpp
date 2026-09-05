#include "platform.h"
#include "studio.h"
#include "relative_time.h"
#include "imgui_internal.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {
const char *themes[] = {
    "Dear ImGui / original blue", "Dear ImGui / classic", "Emerald", "Crimson", "Amber", "Cyan", "Violet"};
const char *fonts[] = {"Roboto Medium", "JetBrains Mono", "Dear ImGui original"};
const char *tabs[] = {"\xef\x80\x87 Accounts", "\xef\x87\xbc Appearance", "\xef\x87\x80 Storage"};
double Now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
void Section(const char *title) {
    ImGui::Spacing();
    ImGui::SeparatorText(title);
}
void CenterCell(float y, float height, float itemHeight) {
    auto p = ImGui::GetCursorScreenPos();
    p.y = y + (height - itemHeight) * .5f;
    ImGui::SetCursorScreenPos(p);
}
void UsageCell(double amount, float y, float height, bool refreshing, float scale,
               const char *loadingText = "Refreshing...", const char *emptyText = "Not checked") {
    float font = ImGui::GetFontSize(), block = font + 8 * scale;
    CenterCell(y, height, block);
    auto p = ImGui::GetCursorScreenPos();
    float width = std::max(40.f, ImGui::GetContentRegionAvail().x);
    auto *draw = ImGui::GetWindowDrawList();
    if (refreshing) {
        float angle = float(ImGui::GetTime() * 4);
        draw->PathClear();
        draw->PathArcTo({p.x + 6 * scale, p.y + font * .5f}, 5 * scale, angle, angle + 4.6f, 18);
        draw->PathStroke(ImGui::GetColorU32(ImGuiCol_SliderGrab), 0, 1.6f * scale);
        draw->AddText({p.x + 17 * scale, p.y}, ImGui::GetColorU32(ImGuiCol_Text), loadingText);
    } else if (amount < 0)
        draw->AddText(p, ImGui::GetColorU32(ImGuiCol_TextDisabled), emptyText);
    else {
        char text[20];
        std::snprintf(text, sizeof(text), "%.0f%%", amount);
        draw->AddText(p, ImGui::GetColorU32(ImGuiCol_Text), text);
    }
    float barY = p.y + font + 5 * scale;
    draw->AddRectFilled({p.x, barY}, {p.x + width, barY + 3 * scale}, ImGui::GetColorU32(ImGuiCol_FrameBg),
                        1);
    if (amount >= 0 && !refreshing)
        draw->AddRectFilled({p.x, barY}, {p.x + width * float(amount / 100), barY + 3 * scale},
                            ImGui::GetColorU32(ImGuiCol_SliderGrab), 1);
    if (refreshing) {
        float x = float(std::fmod(ImGui::GetTime() * .7, 1.0)) * width * .65f;
        draw->AddRectFilled({p.x + x, barY}, {p.x + x + width * .35f, barY + 3 * scale},
                            ImGui::GetColorU32(ImGuiCol_SliderGrab), 1);
    }
    ImGui::Dummy({width, block});
}
bool IconButton(const char *icon, const char *hint, float scale) {
    bool clicked = ImGui::Button(icon, {24 * scale, 0});
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", hint);
    return clicked;
}
void LimitTooltip(const char *label, int64_t resetsAt, bool checking, bool refreshing,
                  const std::string &error) {
    if (!ImGui::IsItemHovered())
        return;
    ImGui::BeginTooltip();
    ImGui::TextUnformatted(label);
    ImGui::Separator();
    ImGui::TextUnformatted(ResetCountdown(resetsAt).c_str());
    if (checking)
        ImGui::TextDisabled("Checking usage...");
    else if (refreshing)
        ImGui::TextDisabled("Refreshing usage...");
    if (!error.empty()) {
        ImGui::Spacing();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 320);
        ImGui::Text("Usage check failed: %s", error.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::EndTooltip();
}
} // namespace
Studio::Studio(std::filesystem::path root, bool preview)
    : root_(std::move(root)), store_(platform::DataDirectory(root_) / "accounts"), preview_(preview) {
    if(!preview_){
        const auto dataRoot=platform::DataDirectory(root_);
        std::filesystem::create_directories(dataRoot/"accounts/profiles");
        std::filesystem::create_directories(dataRoot/"accounts/temp");
        std::filesystem::create_directories(platform::AssetDirectory(root_)/"fonts");
    }
    if (preview_) {
        profiles_ = {
            {"demo1", "Account 1", "#4296FA", "", "pro", "2026-09-05T08:00:00Z", 72, 84, true, true},
            {"demo2", "Account 2", "#AA66FF", "", "plus", "2026-09-05T08:00:00Z", 48, 61, false, true},
            {"demo3", "Account 3", "#33DD99", "", "pro", "2026-09-05T08:00:00Z", 91, 96, false, true}};
        status = "Preview data";
        const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
        for (auto &profile : profiles_) {
            profile.shortResetsAt = now + 7260;
            profile.weeklyResetsAt = now + 3 * 86400 + 7200;
        }
        return;
    }
    refreshSchedule_.Reset(Now());
    LoadSettings(platform::DataDirectory(root_) / "appearance.ini", settings, appearanceStatus_);
    try {
        int imported = store_.ImportLegacy();
        Reload();
        if (imported)
            status = std::to_string(imported) + " saved accounts imported";
        Start([]{OperationResult result("Ready");result.installationInfo=platform::InstallationInfo();return result;});
    } catch (...) {
        status = "Could not read account storage. Check the Storage tab.";
        error_ = true;
    }
}
Studio::~Studio() {
    cancel_ = true;
    if (task_.valid())
        task_.wait();
}
void Studio::Reload() {
    if (preview_)
        return;
    profiles_ = store_.Load();
}
void Studio::Start(std::function<OperationResult()> action, bool login) {
    if (busy_ || preview_)
        return;
    cancel_ = false;
    busy_ = true;
    login_ = login;
    error_ = false;
    refreshingAll_ = false;
    status = login ? "Complete sign-in in your browser" : "Working...";
    task_ = std::async(std::launch::async, [fn = std::move(action), login]() -> OperationResult {
        try {
            return fn();
        } catch (const accounts::OperationCanceled &) {
            OperationResult result(login ? "Sign-in canceled" : "Operation canceled");
            result.canceled = true;
            return result;
        } catch (const accounts::Json::exception &) {
            OperationResult result("Invalid local data or Codex response.");
            result.error = true;
            return result;
        } catch (const std::exception &e) {
            OperationResult result(e.what());
            result.error = true;
            return result;
        } catch (...) {
            OperationResult result("Operation failed.");
            result.error = true;
            return result;
        }
    });
}
void Studio::RefreshAll() {
    if (busy_ || preview_ || profiles_.empty())
        return;
    std::vector<std::string> ids;
    for (const auto &p : profiles_)
        ids.push_back(p.id);
    refreshTotal_ = int(ids.size());
    refreshCompleted_ = 0;
    usageErrors_.clear();
    Start([this, ids]() -> OperationResult {
        OperationResult result("Usage refreshed");
        int succeeded = 0;
        for (const auto &id : ids) {
            if (cancel_.load())
                throw accounts::OperationCanceled();
            try {
                refreshUsage_(store_, id, cancel_);
                ++succeeded;
            } catch (const accounts::OperationCanceled &) {
                throw;
            } catch (const accounts::Json::exception &) {
                result.usageErrors[id] = "Invalid usage response. Retry later.";
            } catch (const std::exception &e) {
                result.usageErrors[id] = e.what();
            }
            ++refreshCompleted_;
        }
        result.message =
            "Usage refreshed: " + std::to_string(succeeded) + "/" + std::to_string(ids.size()) + " accounts";
        result.error = !result.usageErrors.empty();
        if (result.error)
            result.message += ". Hover the marked values for details.";
        return result;
    });
    refreshingAll_ = true;
    status = "Refreshing usage...";
}
void Studio::Tick() {
    if (task_.valid() && task_.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
        auto result = task_.get();
        if(!result.installationInfo.empty())installationInfo_=result.installationInfo;
        status = result.message;
        error_ = result.error;
        if (status.rfind("ERROR: ", 0) == 0) {
            status.erase(0, 7);
            error_ = true;
        }
        if (refreshingAll_)
            usageErrors_ = std::move(result.usageErrors);
        if (!checkingUsageId_.empty()) {
            usageErrors_.erase(checkingUsageId_);
            if (result.error || result.canceled)
                usageErrors_[checkingUsageId_] = result.message;
        }
        const auto addedId = (!result.error && !result.canceled) ? result.addedAccountId : std::string{};
        closeAdd_ = !addedId.empty();
        if (closeAdd_)
            label_[0] = 0;
        if (refreshingAll_)
            refreshSchedule_.Reset(Now());
        busy_ = login_ = refreshingAll_ = switching_ = false;
        checkingUsageId_.clear();
        try {
            Reload();
        } catch (...) {
            status = "Could not reload the saved account list.";
            error_ = true;
        }
        if (!addedId.empty()) {
            search_[0] = 0;
            requestedTab_ = 0;
            CheckNewAccountUsage(addedId);
        }
    }
    if (!preview_ && refreshSchedule_.Due(Now(), busy_, modalOpen_ || openAdd_, !profiles_.empty()))
        RefreshAll();
}
void Studio::CheckNewAccountUsage(const std::string &id) {
    if (busy_ || preview_)
        return;
    auto found = std::find_if(profiles_.begin(), profiles_.end(), [&](const auto &p) { return p.id == id; });
    if (found == profiles_.end()) {
        status = "Account saved, but its usage could not be loaded. Refresh usage to retry.";
        error_ = true;
        return;
    }
    usageErrors_.erase(id);
    Start([this, id]() -> OperationResult {
        refreshUsage_(store_, id, cancel_);
        return "Account saved. Usage checked.";
    });
    checkingUsageId_ = id;
    status = "Account saved. Checking usage...";
}
void Studio::InitFonts() {
    auto &io = ImGui::GetIO();
    static const ImWchar latin[] = {0x20, 0x17F, 0};
    static const ImWchar iconRange[] = {0xF000, 0xF8FF, 0};
    auto merge = [&]() {
        auto p = (platform::AssetDirectory(root_) / "fonts/fa-solid-900.ttf").string();
        if (std::filesystem::exists(p)) {
            ImFontConfig cfg;
            cfg.MergeMode = true;
            cfg.PixelSnapH = true;
            cfg.GlyphMinAdvanceX = 13;
            cfg.GlyphOffset = {0, 1};
            io.Fonts->AddFontFromFileTTF(p.c_str(), 12, &cfg, iconRange);
        }
    };
    const char *files[] = {"Roboto-Medium.ttf", "JetBrainsMono-Regular.ttf"};
    for (int i = 0; i < 2; i++) {
        auto p = (platform::AssetDirectory(root_) / "fonts" / files[i]).string();
        if (std::filesystem::exists(p))
            fonts_[i] = io.Fonts->AddFontFromFileTTF(p.c_str(), 14, nullptr, latin);
        if (!fonts_[i])
            fonts_[i] = io.Fonts->AddFontDefault();
        merge();
    }
    fonts_[2] = io.Fonts->AddFontDefault();
    static const ImWchar ext[] = {0x100, 0x17F, 0};
    auto p = (platform::AssetDirectory(root_) / "fonts/JetBrainsMono-Regular.ttf").string();
    if (std::filesystem::exists(p)) {
        ImFontConfig cfg;
        cfg.MergeMode = true;
        io.Fonts->AddFontFromFileTTF(p.c_str(), 13, &cfg, ext);
    }
    merge();
}
void Studio::ApplyTheme() {
    auto &s = ImGui::GetStyle();
    s = ImGuiStyle();
    if (settings.theme == 1)
        ImGui::StyleColorsClassic();
    else
        ImGui::StyleColorsDark();
    if (settings.theme >= 2) {
        const float hues[] = {0, 0, .39f, .97f, .10f, .51f, .75f};
        for (int i = 0; i < ImGuiCol_COUNT; i++) {
            auto &c = s.Colors[i];
            float h, sat, v;
            ImGui::ColorConvertRGBtoHSV(c.x, c.y, c.z, h, sat, v);
            if (sat > .25f) {
                ImGui::ColorConvertHSVtoRGB(hues[settings.theme], sat, v, c.x, c.y, c.z);
            }
        }
    }
    s.WindowRounding = settings.rounding;
    s.FrameRounding = settings.rounding;
    s.TabRounding = settings.rounding;
    if (settings.density == 0) {
        s.FramePadding = {6, 5};
        s.ItemSpacing = {8, 8};
    } else {
        s.FramePadding = {4, 3};
        s.ItemSpacing = {8, 5};
    }
    s.Colors[ImGuiCol_PlotHistogram] = s.Colors[ImGuiCol_SliderGrab];
    s.Colors[ImGuiCol_TableHeaderBg] = s.Colors[ImGuiCol_FrameBg];
    auto accent = s.Colors[ImGuiCol_SliderGrab];
    s.Colors[ImGuiCol_Border] = ImVec4(accent.x, accent.y, accent.z, .5f);
    s.Colors[ImGuiCol_TableBorderStrong] = ImVec4(accent.x, accent.y, accent.z, .45f);
    s.Colors[ImGuiCol_TableBorderLight] = ImVec4(accent.x, accent.y, accent.z, .2f);
    s.Colors[ImGuiCol_Separator] = ImVec4(accent.x, accent.y, accent.z, .4f);
    s.Colors[ImGuiCol_TableHeaderBg] = s.Colors[ImGuiCol_TitleBgActive];
    s.Colors[ImGuiCol_TabDimmedSelected] = s.Colors[ImGuiCol_TabSelected];
    s.Colors[ImGuiCol_TabDimmed] = s.Colors[ImGuiCol_Tab];
    s.TabBarBorderSize = 0;
    s.Colors[ImGuiCol_PopupBg].w = 1.0f;
    s.ScaleAllSizes(settings.scale);
    ImGui::GetIO().FontDefault = fonts_[settings.font];
    ImGui::GetIO().FontGlobalScale = settings.scale;
}
void Studio::Save() {
    if (preview_)
        return;
    if (SaveSettings(platform::DataDirectory(root_) / "appearance.ini", settings, appearanceStatus_))
        dirty_ = false;
}
std::string Studio::ColorHex() const {
    char buffer[8];
    std::snprintf(buffer, sizeof(buffer), "#%02X%02X%02X", int(std::clamp(color_[0], 0.f, 1.f) * 255),
              int(std::clamp(color_[1], 0.f, 1.f) * 255), int(std::clamp(color_[2], 0.f, 1.f) * 255));
    return buffer;
}
void Studio::Accounts() {
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%d accounts", int(profiles_.size()));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(190 * settings.scale);
    ImGui::InputTextWithHint("##search", "Filter accounts...", search_, sizeof(search_));
    ImGui::SameLine();
    ImGui::BeginDisabled(busy_);
    if (ImGui::Button("\xef\x81\xa7 Add account"))
        openAdd_ = true;
    ImGui::SameLine();
    if (ImGui::Button("\xef\x80\xa1 Refresh usage"))
        RefreshAll();
    ImGui::EndDisabled();
    ImGui::Spacing();
    std::string popup, moveFrom, moveTo;
    bool moveAfter = false;
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {10 * settings.scale, 8 * settings.scale});
    float tableWidth = std::max(ImGui::GetContentRegionAvail().x, 875 * settings.scale);
    if (ImGui::BeginTable("account_table", 5,
                          ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersInnerH |
                              ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX |
                              ImGuiTableFlags_SizingFixedFit,
                          {0, -1}, tableWidth)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Account", ImGuiTableColumnFlags_WidthStretch, 1);
        ImGui::TableSetupColumn("Plan", ImGuiTableColumnFlags_WidthFixed, 52 * settings.scale);
        ImGui::TableSetupColumn("5h limit left", ImGuiTableColumnFlags_WidthFixed, 144 * settings.scale);
        ImGui::TableSetupColumn("Weekly left", ImGuiTableColumnFlags_WidthFixed, 144 * settings.scale);
        ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 147 * settings.scale);
        ImGui::TableHeadersRow();
        int shown = 0;
        for (auto &p : profiles_) {
            std::string name = p.label, query = search_;
            auto lower = [](unsigned char c) { return char(tolower(c)); };
            std::transform(name.begin(), name.end(), name.begin(), lower);
            std::transform(query.begin(), query.end(), query.begin(), lower);
            if (name.find(query) == std::string::npos)
                continue;
            shown++;
            bool refreshing = refreshingAll_ || (preview_ && captureState == "refresh");
            const bool checking = (busy_ && checkingUsageId_ == p.id) ||
                                  (preview_ && captureState == "checking" && p.id == "demo3");
            const std::string usageError = usageErrors_.count(p.id) ? usageErrors_.at(p.id) : std::string{};
            const char *emptyText = !usageError.empty()    ? "Check failed"
                                    : !p.checkedAt.empty() ? "Unavailable"
                                                           : "Not checked";
            ImGui::PushID(p.id.c_str());
            float inner = 32 * settings.scale;
            ImGui::TableNextRow(ImGuiTableRowFlags_None, inner + 16 * settings.scale);
            ImGui::TableNextColumn();
            float y = ImGui::GetCursorScreenPos().y;
            const ImVec2 cell = ImGui::GetCursorScreenPos();
            const float width = ImGui::GetContentRegionAvail().x;
            ImGui::BeginDisabled(busy_);
            ImGui::InvisibleButton("drag_account", {width, inner});
            bool hovered = ImGui::IsItemHovered();
            auto *draw = ImGui::GetWindowDrawList();
            float textY = y + (inner - ImGui::GetTextLineHeight() * 2 - 2 * settings.scale) * .5f;
            draw->AddText({cell.x, y + (inner - ImGui::GetTextLineHeight()) * .5f},
                          ImGui::GetColorU32(hovered ? ImGuiCol_SliderGrab : ImGuiCol_TextDisabled),
                          "\xef\x96\x8e");
            ImVec2 textPos(cell.x + 20 * settings.scale, textY);
            draw->PushClipRect({textPos.x, cell.y}, {cell.x + width, cell.y + inner}, true);
            draw->AddText(textPos, ImGui::GetColorU32(ImGuiCol_Text), p.label.c_str());
            draw->AddText({textPos.x, textY + ImGui::GetTextLineHeight() + 2 * settings.scale},
                          ImGui::GetColorU32(p.active ? ImGuiCol_SliderGrab : ImGuiCol_TextDisabled),
                          p.active      ? "\xef\x80\x8c Active"
                          : p.available ? "Saved account"
                                        : "Credential file missing");
            draw->PopClipRect();
            if (hovered)
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoDisableHover)) {
                ImGui::SetDragDropPayload("CODEX_ACCOUNT_ORDER", p.id.c_str(), p.id.size() + 1);
                ImGui::Text("Move: %s", p.label.c_str());
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginDragDropTarget()) {
                if (const auto *payload = ImGui::AcceptDragDropPayload(
                        "CODEX_ACCOUNT_ORDER", ImGuiDragDropFlags_AcceptBeforeDelivery |
                                                   ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
                    bool after = ImGui::GetIO().MousePos.y > cell.y + inner * .5f;
                    float lineY = after ? cell.y + inner + 7 * settings.scale : cell.y - 7 * settings.scale;
                    draw->AddLine({cell.x, lineY}, {cell.x + width, lineY},
                                  ImGui::GetColorU32(ImGuiCol_SliderGrab), 2 * settings.scale);
                    if (payload->IsDelivery() && payload->DataSize > 1 && payload->DataSize <= 33) {
                        const char *raw = static_cast<const char *>(payload->Data);
                        if (raw[payload->DataSize - 1] == 0) {
                            moveFrom = std::string(raw, size_t(payload->DataSize - 1));
                            moveTo = p.id;
                            moveAfter = after;
                        }
                    }
                }
                ImGui::EndDragDropTarget();
            }
            if (hovered && !ImGui::GetDragDropPayload())
                ImGui::SetTooltip("%s\nDrag up or down to reorder", p.label.c_str());
            ImGui::EndDisabled();
            ImGui::TableNextColumn();
            CenterCell(y, inner, ImGui::GetTextLineHeight());
            ImGui::TextUnformatted(p.plan.empty() ? "--" : p.plan.c_str());
            ImGui::TableNextColumn();
            UsageCell(p.shortTerm, y, inner, refreshing || checking, settings.scale,
                      checking ? "Checking usage..." : "Refreshing...", emptyText);
            if (!refreshing && usageErrors_.count(p.id)) {
                auto warningPos = ImGui::GetItemRectMin();
                ImGui::GetWindowDrawList()->AddText({warningPos.x + 42 * settings.scale, warningPos.y},
                                                    ImGui::GetColorU32(ImGuiCol_SliderGrab), "!");
            }
            LimitTooltip("5h limit", p.shortResetsAt, checking, refreshing, usageError);
            ImGui::TableNextColumn();
            UsageCell(p.weekly, y, inner, refreshing || checking, settings.scale,
                      checking ? "Checking usage..." : "Refreshing...", emptyText);
            LimitTooltip("Weekly limit", p.weeklyResetsAt, checking, refreshing, usageError);
            ImGui::TableNextColumn();
            CenterCell(y, inner, ImGui::GetFrameHeight());
            ImGui::BeginDisabled(busy_ || (preview_ && captureState == "refresh"));
            ImGui::BeginDisabled(p.active || !p.available);
            if (ImGui::Button(p.active ? "Active" : "Switch", {65 * settings.scale, 0})) {
                pendingId_ = p.id;
                pendingName_ = p.label;
                popup = "Switch account?";
            }
            ImGui::EndDisabled();
            ImGui::SameLine(0, 5 * settings.scale);
            if (IconButton("\xef\x81\x80", "Rename account", settings.scale)) {
                pendingId_ = p.id;
                pendingColor_ = p.color;
                std::snprintf(rename_,sizeof(rename_),"%s",p.label.c_str());
                popup = "Rename account";
            }
            ImGui::SameLine(0, 5 * settings.scale);
            if (IconButton("\xef\x87\xb8", "Remove saved account", settings.scale)) {
                pendingId_ = p.id;
                pendingName_ = p.label;
                popup = "Remove saved account?";
            }
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        if (!shown) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", profiles_.empty() ? "No saved accounts. Add an account to begin."
                                                       : "No matching accounts.");
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
    if (!moveFrom.empty() && moveFrom != moveTo) {
        if (preview_) {
            auto source =
                std::find_if(profiles_.begin(), profiles_.end(), [&](auto &p) { return p.id == moveFrom; });
            if (source != profiles_.end()) {
                auto item = *source;
                profiles_.erase(source);
                auto anchor =
                    std::find_if(profiles_.begin(), profiles_.end(), [&](auto &p) { return p.id == moveTo; });
                if (anchor != profiles_.end()) {
                    if (moveAfter)
                        ++anchor;
                    profiles_.insert(anchor, item);
                }
            }
        } else
            Start([this, moveFrom, moveTo, moveAfter] {
                store_.Move(moveFrom, moveTo, moveAfter);
                return "Account order saved";
            });
    }
    if (!popup.empty())
        ImGui::OpenPopup(popup.c_str());
    if (openAdd_ ||
        (preview_ && (captureState == "add" || captureState == "login" || captureState == "canceling") &&
         ImGui::GetFrameCount() == 2)) {
        ImGui::OpenPopup("Add account");
        closeAdd_ = false;
        error_ = false;
        openAdd_ = false;
    }
    Dialogs();
    modalOpen_ = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) ||
                 ImGui::GetDragDropPayload() != nullptr;
}
void Studio::Dialogs() {
    AddAccount();
    ImGui::SetNextWindowSize({460 * settings.scale, 0}, ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Switch account?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("Switch to %s?", pendingName_.c_str());
        ImGui::Spacing();
        ImGui::TextWrapped("Codex will close and restart with this account. Save unfinished work before "
                           "confirming. Your current sign-in will be backed up.");
        ImGui::Spacing();
        if (ImGui::Button("Confirm switch")) {
            auto id = pendingId_;
            Start([this, id] { return accounts::SwitchInHelper(id); });
            switching_ = true;
            status = "Closing Codex and switching account...";
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("Rename account", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputText("Name", rename_, sizeof(rename_));
        if (ImGui::Button("Save")) {
            auto id = pendingId_, label = std::string(rename_);
            auto color = pendingColor_;
            Start([this, id, label, color] {
                store_.Rename(id, label, color);
                return "Account renamed";
            });
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::SetNextWindowSize({430 * settings.scale, 0}, ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Remove saved account?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("Remove %s from this application's saved accounts?", pendingName_.c_str());
        ImGui::TextWrapped("The local encrypted copy will be deleted. The active Codex sign-in "
                           "and the original WPF store are unchanged.");
        ImGui::Spacing();
        if (ImGui::Button("Remove")) {
            auto id = pendingId_;
            Start([this, id] {
                store_.Remove(id);
                return "Saved account removed";
            });
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}
void Studio::AddAccount() {
    // BeginPopupModal sets p_open=false when the popup is NOT open.
    // Never treat that as a user cancellation of a different background operation.
    const bool wasOpen = ImGui::IsPopupOpen("Add account");
    addVisible_ = wasOpen;
    if (!wasOpen)
        return;
    auto *vp = ImGui::GetMainViewport();
    const ImVec2 size(std::min(400 * settings.scale, vp->WorkSize.x - 24),
                      std::min(218 * settings.scale, vp->WorkSize.y - 24));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, {.5f, .5f});
    bool open = true;
    const bool simulatedLogin = preview_ && (captureState == "login" || captureState == "canceling");
    const bool waiting = (busy_ && login_) || simulatedLogin;
    const bool cancelling = (login_ && cancel_.load()) || (preview_ && captureState == "canceling");
    if (ImGui::BeginPopupModal("Add account", &open,
                               ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings)) {
        if (closeAdd_) {
            closeAdd_ = false;
            ImGui::CloseCurrentPopup();
        }
        if (waiting) {
            ImGui::Spacing();
            ImGui::TextWrapped("%s",
                               cancelling ? "Cancelling sign-in..." : "Complete sign-in in your browser");
            ImGui::Spacing();
            ImGui::TextWrapped("%s", cancelling ? "Closing the login session..."
                                                : "Your account will be saved when sign-in finishes.");
            float y = ImGui::GetCursorScreenPos().y;
            UsageCell(-1, y, 40 * settings.scale, true, settings.scale,
                      cancelling ? "Cancelling..." : "Waiting for sign-in...");
            ImGui::Spacing();
            ImGui::BeginDisabled(cancelling);
            if (ImGui::Button(cancelling ? "Cancelling..." : "Cancel sign-in", {170 * settings.scale, 0})) {
                cancel_ = true;
                status = "Cancelling sign-in...";
            }
            ImGui::EndDisabled();
        } else {
            ImGui::BeginDisabled(busy_);
            ImGui::TextUnformatted("Account name");
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##accountname", "Choose a private name", label_, sizeof(label_));
            ImGui::Spacing();
            const bool empty = std::string(label_).find_first_not_of(" \t\r\n") == std::string::npos;
            ImGui::BeginDisabled(empty);
            if (ImGui::Button("\xef\x82\x90 Sign in with browser", {-1, 0})) {
                auto name = std::string(label_), color = ColorHex();
                Start(
                    [this, name, color] {
                        OperationResult result("New account saved");
                        result.addedAccountId = accounts::Login(store_, name, color, cancel_);
                        return result;
                    },
                    true);
            }
            ImGui::EndDisabled();
            ImGui::EndDisabled();
            ImGui::Spacing();
            if (error_)
                ImGui::TextWrapped("%s", status.c_str());
            else
                ImGui::TextWrapped(
                    "Sign in with your OpenAI account. It will be added to this list automatically.");
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            open = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (wasOpen && !open && busy_ && login_) {
        cancel_ = true;
        status = "Cancelling sign-in...";
    }
}
void Studio::Appearance() {
    Section("Dear ImGui style");
    ImGui::SetNextItemWidth(300 * settings.scale);
    dirty_ |= ImGui::Combo("Theme", &settings.theme, themes, 7);
    ImGui::SetNextItemWidth(300 * settings.scale);
    dirty_ |= ImGui::Combo("Font", &settings.font, fonts, 3);
    ImGui::SetNextItemWidth(300 * settings.scale);
    dirty_ |= ImGui::SliderFloat("UI scale", &settings.scale, .85f, 1.6f, "%.2fx");
    ImGui::SetNextItemWidth(300 * settings.scale);
    dirty_ |= ImGui::SliderFloat("Rounding", &settings.rounding, 0, 8, "%.0f px");
    ImGui::SetNextItemWidth(300 * settings.scale);
    dirty_ |= ImGui::Combo("Spacing", &settings.density, "Comfortable\0Original / compact\0");
    ImGui::Spacing();
    if (ImGui::Button("Restore original Dear ImGui")) {
        settings = Settings{};
        dirty_ = true;
    }
    Section("Font preview");
    ImGui::TextUnformatted("The quick brown fox / 0123456789");
    ImGui::TextUnformatted("ABCDEFGHIJKLMNOPQRSTUVWXYZ / abcdefghijklmnopqrstuvwxyz");
    ImGui::TextUnformatted("\xef\x80\x87   \xef\x87\x80   \xef\x83\x87   "
                           "\xef\x80\xa1   \xef\x80\x8c   \xef\x80\x8d");
    ImGui::Spacing();
    ImGui::TextWrapped("Font changes and themes apply immediately. Save "
                       "appearance to keep them for the next launch.");
    if (ImGui::Button("Save appearance"))
        Save();
    if (dirty_)
        ImGui::TextDisabled("Appearance changes not saved");
    if (!appearanceStatus_.empty())
        ImGui::TextWrapped("%s", appearanceStatus_.c_str());
}
void Studio::Storage() {
    Section("Account storage");
    ImGui::TextWrapped("%s", store_.root.string().c_str());
    ImGui::TextWrapped("Existing WPF profiles are imported once as encrypted copies. This "
                       "application resolves storage independently of the current working directory.");
    ImGui::BeginDisabled(busy_ || preview_);
    if (ImGui::Button("Open storage folder")) {
        auto p = store_.root;
        std::filesystem::create_directories(p);
        try{platform::OpenFolder(p);}catch(const std::exception& e){status=e.what();error_=true;}
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload accounts")) {
        try {
            Reload();
            status = "Accounts reloaded";
        } catch (...) {
            status = "Could not load account storage.";
            error_ = true;
        }
    }
    ImGui::EndDisabled();
    Section("Codex installation");
    if(!installationInfo_.empty())ImGui::TextWrapped("%s",installationInfo_.c_str());
    ImGui::BeginDisabled(busy_||preview_);
    if(ImGui::Button("Detect installation"))Start([]{OperationResult result("Codex installation detected");result.installationInfo=platform::InstallationInfo();return result;});
    ImGui::EndDisabled();
    ImGui::TextWrapped("Active authentication: %s", accounts::ActiveAuth().string().c_str());
    ImGui::BeginDisabled(busy_ || preview_);
    if (ImGui::Button("Check Codex connection"))
        Start([this] {
            return accounts::Probe(cancel_) ? "Codex App Server connection succeeded"
                                            : "ERROR: Codex connection failed";
        });
    ImGui::SameLine();
    if (ImGui::Button("Open Codex")) {
        try {
            accounts::LaunchCodex();
            status = "Codex launch requested";
        } catch (const std::exception &e) {
            status = e.what();
            error_ = true;
        }
    }
    ImGui::EndDisabled();
    Section("About");
    ImGui::Text("C++ / Dear ImGui %s", IMGUI_VERSION);
    ImGui::TextWrapped("Manual account switching. "
                       "Network sign-in and usage requests run through your installed Codex CLI. Saved profiles use the operating system credential store.");
}
void Studio::Draw() {
    ApplyTheme();
    auto *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    bool open = true;
    bool visible = ImGui::Begin("Codex Switcher", &open,
                                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar);
    if (!open) {
        cancel_ = true;
        exitRequested = true;
    }
    if (visible) {
        if (ImGui::BeginMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("Save appearance", "Ctrl+S"))
                    Save();
                if (ImGui::MenuItem("Close", "Alt+F4")) {
                    cancel_ = true;
                    exitRequested = true;
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Accounts")) {
                if (ImGui::MenuItem("Saved accounts"))
                    requestedTab_ = 0;
                if (ImGui::MenuItem("Add account", nullptr, false, !busy_)) {
                    requestedTab_ = 0;
                    openAdd_ = true;
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Themes")) {
                for (int i = 0; i < 7; i++)
                    if (ImGui::MenuItem(themes[i], nullptr, settings.theme == i)) {
                        settings.theme = i;
                        dirty_ = true;
                    }
                ImGui::EndMenu();
            }
            ImGui::EndMenuBar();
        }
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S))
            Save();
        if (ImGui::BeginTabBar("main_tabs", ImGuiTabBarFlags_FittingPolicyScroll)) {
            int request = requestedTab_;
            requestedTab_ = -1;
            for (int i = 0; i < 3; i++)
                if (ImGui::BeginTabItem(tabs[i], nullptr,
                                        forcedTab == i || request == i ? ImGuiTabItemFlags_SetSelected : 0)) {
                    float footer = ImGui::GetFrameHeightWithSpacing() * 2;
                    ImGui::BeginChild("body", {0, -footer}, ImGuiChildFlags_None);
                    if (i == 0)
                        Accounts();
                    else if (i == 1)
                        Appearance();
                    else
                        Storage();
                    ImGui::EndChild();
                    ImGui::EndTabItem();
                }
            ImGui::EndTabBar();
        }
        ImGui::TextDisabled("Auto refresh: every 5 minutes");
        if (busy_ || (preview_ && captureState == "refresh")) {
            ImGui::AlignTextToFramePadding();
            if (refreshingAll_ || (preview_ && captureState == "refresh"))
                ImGui::Text("Refreshing usage... %d / %d", refreshCompleted_.load(),
                            preview_ ? 3 : refreshTotal_);
            else if (login_) {
                ImGui::TextUnformatted(cancel_.load() ? "Cancelling sign-in..."
                                                      : "Complete sign-in in your browser");
                if (!addVisible_) {
                    ImGui::SameLine();
                    ImGui::BeginDisabled(cancel_.load());
                    if (ImGui::SmallButton("Cancel sign-in"))
                        cancel_ = true;
                    ImGui::EndDisabled();
                }
            } else
                ImGui::TextUnformatted(switching_ ? "Closing Codex, switching account and restarting..."
                                                  : status.c_str());
        } else {
            if (error_)
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, .4f, .35f, 1));
            ImGui::TextWrapped("%s", status.c_str());
            if (error_)
                ImGui::PopStyleColor();
        }
    }
    ImGui::End();
}

bool Studio::UiSelfTest() {
    RefreshSchedule schedule;
    schedule.Reset(100);
    bool ok = !schedule.Due(399, false, false, true) && schedule.Due(400, false, false, true) &&
              !schedule.Due(500, true, false, true) && !schedule.Due(500, false, true, true) &&
              !schedule.Due(500, false, false, false);
    schedule.Reset(450);
    ok = ok && !schedule.Due(749, false, false, true) && schedule.Due(750, false, false, true);
    ImGui::CreateContext();
    {
        Studio app(std::filesystem::temp_directory_path(), true);
        auto &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = {1000, 560};
        io.DeltaTime = 1.f / 60;
        app.InitFonts();
        io.Fonts->Build();
        auto frame = [&] {
            ImGui::NewFrame();
            app.Draw();
            ImGui::Render();
        };
        // A closed Add account modal must not cancel a refresh on any frame.
        app.busy_ = true;
        app.refreshingAll_ = true;
        app.cancel_ = false;
        for (int i = 0; i < 5; i++) {
            frame();
            ok = ok && !app.cancel_.load();
        }
        app.busy_ = app.refreshingAll_ = false;
        app.openAdd_ = true;
        frame();
        auto *modal = ImGui::FindWindowByName("Add account");
        if (!modal)
            ok = false;
        else {
            auto size = modal->Size;
            frame();
            ok = ok && modal->Size.x == size.x && modal->Size.y == size.y;
            app.busy_ = app.login_ = true;
            frame();
            ok = ok && !app.cancel_.load() && modal->Size.x == size.x && modal->Size.y == size.y;
            app.cancel_ = true;
            frame();
            ok = ok && modal->Size.x == size.x && modal->Size.y == size.y;
        }
        app.busy_ = app.login_ = false;
    }
    ImGui::DestroyContext();
    // Exercise the real login-completion -> per-account check transition with a gated mock request.
    // No browser, credentials or service requests are involved.
    const auto testRoot = std::filesystem::temp_directory_path() / accounts::Wide(accounts::Guid());
    {
        Studio app(testRoot, true);
        app.preview_ = false;
        app.profiles_.clear();
        app.refreshSchedule_.Reset(Now());
        const auto id =
            app.store_.Add("New account", "#4296FA",
                           R"({"tokens":{"account_id":"synthetic-new","access_token":"synthetic"}})");
        auto gate = std::make_shared<std::promise<void>>();
        auto released = gate->get_future().share();
        app.refreshUsage_ = [released](accounts::Store store, const std::string &accountId,
                                       std::atomic_bool &) {
            released.wait();
            store.UpdateUsage(
                accountId,
                accounts::Json{{"shortTerm", {{"remainingPercent", 88}, {"resetsAt", 1788613200LL}}},
                               {"weekly", {{"remainingPercent", 77}, {"resetsAt", 1788872400LL}}}},
                "");
        };
        std::promise<OperationResult> login;
        OperationResult result("New account saved");
        result.addedAccountId = id;
        app.task_ = login.get_future();
        app.busy_ = app.login_ = true;
        login.set_value(result);
        app.Tick();
        ok = ok && app.busy_ && !app.login_ && !app.refreshingAll_ && app.checkingUsageId_ == id &&
             app.closeAdd_ && app.profiles_.size() == 1 && app.profiles_[0].shortTerm < 0 &&
             !app.cancel_.load();
        gate->set_value();
        app.task_.wait();
        app.Tick();
        ok = ok && !app.busy_ && app.checkingUsageId_.empty() && app.profiles_[0].shortTerm == 88 &&
             app.profiles_[0].weekly == 77;
        app.refreshUsage_ = [](accounts::Store, const std::string &, std::atomic_bool &) {
            throw std::runtime_error("Service unavailable");
        };
        app.CheckNewAccountUsage(id);
        app.task_.wait();
        app.Tick();
        ok = ok && !app.busy_ && app.checkingUsageId_.empty() && app.usageErrors_.count(id) == 1 &&
             app.store_.Load(false).size() == 1;
    }
    std::error_code cleanup;
    std::filesystem::remove_all(testRoot, cleanup);
    return ok;
}
