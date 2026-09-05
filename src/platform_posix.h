#pragma once
#include "accounts.h"
#include <vector>
namespace platform {
std::string RunCapture(const accounts::Path& executable,const std::vector<std::string>& args,int timeoutSeconds=8);
void Spawn(const accounts::Path& executable,const std::vector<std::string>& args);
accounts::Path FindDesktop();
}
