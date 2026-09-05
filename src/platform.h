#pragma once
#include <filesystem>
#include <string>
#include <cstddef>
namespace platform {
using Path=std::filesystem::path;
Path Executable();
Path DataDirectory(const Path& executableDirectory);
Path AssetDirectory(const Path& executableDirectory);
void OpenFolder(const Path& path);
void OpenUrl(const std::string& url);
void Erase(void* data,size_t size);
bool RemoveFile(const Path& path);
std::string InstallationInfo();
}
