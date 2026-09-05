#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <appmodel.h>
#include "platform.h"
#include "accounts.h"
#include <vector>
#include <cstdlib>
namespace platform {
Path Executable(){wchar_t path[32768];DWORD n=GetModuleFileNameW(nullptr,path,32768);if(!n||n==32768)throw std::runtime_error("Could not resolve executable path.");return path;}
Path DataDirectory(const Path& root){
    return root/"data";
}
Path AssetDirectory(const Path& root){return root/"assets";}
void Erase(void* data,size_t size){if(size)SecureZeroMemory(data,size);}
bool RemoveFile(const Path& path){return DeleteFileW(path.c_str())!=0;}
void OpenFolder(const Path& path){if(reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr,L"open",path.c_str(),nullptr,nullptr,SW_SHOWNORMAL))<=32)throw std::runtime_error("Could not open the folder.");}
void OpenUrl(const std::string& url){auto text=accounts::Wide(url);if(reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr,L"open",text.c_str(),nullptr,nullptr,SW_SHOWNORMAL))<=32)throw std::runtime_error("Could not open the browser.");}
static std::string FileVersion(const Path& path){
    DWORD unused=0,n=GetFileVersionInfoSizeW(path.c_str(),&unused);if(!n)return "not reported";
    std::vector<BYTE> bytes(n);if(!GetFileVersionInfoW(path.c_str(),0,n,bytes.data()))return "not reported";
    VS_FIXEDFILEINFO* info=nullptr;UINT length=0;if(!VerQueryValueW(bytes.data(),L"\\",reinterpret_cast<void**>(&info),&length)||length<sizeof(*info))return "not reported";
    return std::to_string(HIWORD(info->dwFileVersionMS))+"."+std::to_string(LOWORD(info->dwFileVersionMS))+"."+std::to_string(HIWORD(info->dwFileVersionLS))+"."+std::to_string(LOWORD(info->dwFileVersionLS));
}
std::string InstallationInfo(){
    std::string result="Codex desktop: not found";UINT32 count=0,length=0;
    LONG code=GetPackagesByPackageFamily(L"OpenAI.Codex_2p2nqsd0c76g0",&count,nullptr,&length,nullptr);
    if(code==ERROR_INSUFFICIENT_BUFFER&&count){std::vector<PWSTR> names(count);std::vector<wchar_t> buffer(length);
        if(GetPackagesByPackageFamily(L"OpenAI.Codex_2p2nqsd0c76g0",&count,names.data(),&length,buffer.data())==ERROR_SUCCESS){
            result="Codex desktop packages:";for(UINT32 i=0;i<count;i++)result+="\n"+accounts::Utf8(names[i]);
        }
    }
    try{auto cli=accounts::FindCli();result+="\nCLI: "+cli.string()+"\nCLI file version: "+FileVersion(cli);}catch(const std::exception& e){result+="\n"+std::string(e.what());}
    return result;
}
}
