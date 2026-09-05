static Json Usage(const Json &reply) {
    Json bucket;
    if (reply.contains("rateLimitsByLimitId") && reply["rateLimitsByLimitId"].is_object()) {
        auto &by = reply["rateLimitsByLimitId"];
        if (by.contains("codex"))
            bucket = by["codex"];
        else if (!by.empty())
            bucket = by.begin().value();
    }
    if (!bucket.is_object() && reply.contains("rateLimits"))
        bucket = reply["rateLimits"];
    Json result = {{"status", "unavailable"}, {"checkedAt", Timestamp()}};
    if (!bucket.is_object())
        return result;
    if (bucket.contains("planType"))
        result["planType"] = bucket["planType"];
    for (auto key : {"primary", "secondary"}) {
        if (!bucket.contains(key) || !bucket[key].is_object())
            continue;
        auto w = bucket[key];
        if (!w.contains("usedPercent") || !w["usedPercent"].is_number())
            continue;
        double used = w["usedPercent"].get<double>();
        if (!std::isfinite(used))
            continue;
        int duration = w.value("windowDurationMins", 0);
        Json window = {{"usedPercent", used},
                       {"remainingPercent", std::clamp(100. - used, 0., 100.)},
                       {"windowDurationMinutes", duration}};
        if (w.contains("resetsAt"))
            window["resetsAt"] = w["resetsAt"];
        result[duration >= 4320 ? "weekly" : "shortTerm"] = window;
        result["status"] = "available";
    }
    return result;
}
std::string Login(Store store, const std::string &label, const std::string &color, std::atomic_bool &cancel) {
    Session s(store.root / L"temp", cancel, "");
    auto result =
        s.Request("account/login/start",
                  {{"type", "chatgpt"}, {"useHostedLoginSuccessPage", true}, {"appBrand", "chatgpt"}});
    auto url = result.value("authUrl", "");
    std::string lower = url;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return char(tolower(c)); });
    Require(lower.rfind("https://auth.openai.com/", 0) == 0, "Codex returned an unexpected sign-in address.");
    platform::OpenUrl(url);
    auto completed = s.WaitLogin();
    Require(completed.value("success", false), "Sign-in was not completed.");
    if (completed.contains("loginId") && result.contains("loginId"))
        Require(completed["loginId"] == result["loginId"], "Sign-in request mismatch.");
    if (cancel.load())
        throw OperationCanceled();
    auto auth = s.Auth();
    std::string addedId;
    try {
        addedId = store.Add(label, color, auth);
    } catch (...) {
        platform::Erase(auth.data(), auth.size());
        throw;
    }
    platform::Erase(auth.data(), auth.size());
    return addedId;
}
void Refresh(Store store, const std::string &id, std::atomic_bool &cancel) {
    auto auth = store.Auth(id);
    try {
        Session s(store.root / L"temp", cancel, auth);
        platform::Erase(auth.data(), auth.size());
        auto usage = Usage(s.Request("account/rateLimits/read"));
        auto updated = s.Auth();
        try {
            store.UpdateUsage(id, usage, updated);
        } catch (...) {
            platform::Erase(updated.data(), updated.size());
            throw;
        }
        platform::Erase(updated.data(), updated.size());
    } catch (...) {
        platform::Erase(auth.data(), auth.size());
        throw;
    }
}
bool Probe(std::atomic_bool &cancel) {
    Session s(std::filesystem::temp_directory_path() / L"CodexImGuiProbe", cancel, "");
    auto result = s.Request("account/read", {{"refreshToken", false}});
    return result.is_object();
}
