#include "accounts.h"
#include "platform.h"
#include "platform_posix.h"
#include <unistd.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <cerrno>
#include <algorithm>
#include <chrono>
#ifdef __APPLE__
#include <libproc.h>
#else
#include <dirent.h>
#endif
extern char** environ;
namespace accounts {
namespace {
std::vector<pid_t> Pids(){
    std::vector<pid_t> result;
#ifdef __APPLE__
    int bytes=proc_listpids(PROC_ALL_PIDS,0,nullptr,0);if(bytes<=0)throw std::runtime_error("Could not enumerate processes.");std::vector<pid_t> pids(size_t(bytes)/sizeof(pid_t)+64);bytes=proc_listpids(PROC_ALL_PIDS,0,pids.data(),int(pids.size()*sizeof(pid_t)));if(bytes<=0)throw std::runtime_error("Could not enumerate processes.");pids.resize(size_t(bytes)/sizeof(pid_t));for(auto pid:pids)if(pid>0)result.push_back(pid);
#else
    DIR* dir=opendir("/proc");if(!dir)throw std::runtime_error("Could not enumerate processes.");while(auto* e=readdir(dir)){std::string name=e->d_name;if(name.empty()||!std::all_of(name.begin(),name.end(),[](unsigned char c){return isdigit(c)!=0;}))continue;try{result.push_back(pid_t(std::stol(name)));}catch(...){}}closedir(dir);
#endif
    return result;
}
Path Image(pid_t pid){
#ifdef __APPLE__
    char path[PROC_PIDPATHINFO_MAXSIZE];int n=proc_pidpath(pid,path,sizeof(path));if(n<=0)return {};return path;
#else
    char path[32768];ssize_t n=readlink(("/proc/"+std::to_string(pid)+"/exe").c_str(),path,sizeof(path)-1);if(n<=0)return {};return Path(std::string(path,size_t(n)));
#endif
}
bool OwnProcess(pid_t pid){
#ifdef __APPLE__
    proc_bsdinfo info{};return proc_pidinfo(pid,PROC_PIDTBSDINFO,0,&info,sizeof(info))==sizeof(info)&&info.pbi_uid==getuid();
#else
    struct stat st{};return stat(("/proc/"+std::to_string(pid)).c_str(),&st)==0&&st.st_uid==getuid();
#endif
}
bool Belongs(const Path& image,const Path& desktop){if(image.empty()||desktop.empty())return false;
#ifdef __APPLE__
    auto prefix=desktop.string()+"/Contents/";return image.string().rfind(prefix,0)==0;
#else
    if(image==desktop)return true;
    // Include only executables in a dedicated app directory, never an entire system bin directory.
    auto parent=desktop.parent_path();if(parent=="/usr/bin"||parent=="/usr/local/bin"||parent.filename()=="bin")return false;
    return image.string().rfind(parent.string()+"/",0)==0;
#endif
}
std::vector<std::pair<pid_t,Path>> Running(const Path& desktop){std::vector<std::pair<pid_t,Path>> result;for(auto pid:Pids())if(pid!=getpid()&&OwnProcess(pid)){auto image=Image(pid);if(Belongs(image,desktop))result.emplace_back(pid,image);}return result;}
bool Alive(const std::pair<pid_t,Path>& p){return OwnProcess(p.first)&&Image(p.first)==p.second&&kill(p.first,0)==0;}
void Stop(const Path& desktop){auto initial=Running(desktop);for(auto& p:initial)if(Alive(p))kill(p.first,SIGTERM);auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);while(std::chrono::steady_clock::now()<deadline&&std::any_of(initial.begin(),initial.end(),Alive))usleep(100000);
    for(auto& p:initial)if(Alive(p)&&kill(p.first,SIGKILL)!=0)throw std::runtime_error("Codex could not be closed. Sign-in was not changed.");
    int empty=0;for(int i=0;i<30;i++){auto current=Running(desktop);if(current.empty()){if(++empty==3)return;}else{empty=0;for(auto& p:current)if(Alive(p))kill(p.first,SIGTERM);}usleep(200000);}throw std::runtime_error("Codex kept restarting. Retry when it has closed.");
}
void Launch(const Path& desktop){
#ifdef __APPLE__
    platform::Spawn("/usr/bin/open",{"-a",desktop.string()});
#else
    platform::Spawn(desktop,{});
#endif
    for(int i=0;i<75;i++){if(!Running(desktop).empty())return;usleep(200000);}throw std::runtime_error("Codex did not reopen automatically. Open it manually.");
}
void Validate(const std::string& id){if(id.size()!=32||!std::all_of(id.begin(),id.end(),[](unsigned char c){return isxdigit(c)!=0;}))throw std::runtime_error("Invalid account selection.");}
}
bool CodexIsRunning(){auto desktop=platform::FindDesktop();if(!desktop.empty()&&!Running(desktop).empty())return true;
    // With CLI-only installations, do not overwrite auth while another CLI process is active.
    try{auto cli=FindCli();for(auto pid:Pids())if(pid!=getpid()&&OwnProcess(pid)&&Image(pid)==cli)return true;}catch(...){}return false;
}
void LaunchCodex(){auto desktop=platform::FindDesktop();if(desktop.empty())throw std::runtime_error("Codex desktop was not found. Open Codex CLI from your terminal.");Launch(desktop);}
int SwitchHelper(const std::string& id,bool dryRun){if(dryRun)return 0;auto root=platform::DataDirectory(platform::Executable().parent_path())/"accounts";std::filesystem::create_directories(root);int lock=open((root/"switch.lock").c_str(),O_RDWR|O_CREAT|O_CLOEXEC,0600);if(lock<0||flock(lock,LOCK_EX|LOCK_NB)){if(lock>=0)close(lock);return 3;}
    bool error=false,changed=false;std::string message;Path desktop;
    try{Validate(id);Store store(root);auto auth=store.Auth(id);platform::Erase(auth.data(),auth.size());desktop=platform::FindDesktop();
        auto config=ActiveAuth().parent_path()/"config.toml";
        if(std::filesystem::exists(config)){auto text=Read(config);if(text.find("cli_auth_credentials_store")!=std::string::npos&&text.find("keyring")!=std::string::npos)throw std::runtime_error("File-based Codex authentication is required for switching. Set cli_auth_credentials_store to file in Codex config.");}
        if(!desktop.empty()){Stop(desktop);Stop(desktop);}store.Activate(id,ActiveAuth(),true);changed=true;
        if(!desktop.empty())Launch(desktop);message=desktop.empty()?"Account switched. Open Codex CLI when ready.":"Account switched. Codex reopened.";
    }catch(const Json::exception&){error=true;message="Invalid saved account data.";}catch(const std::exception& e){error=true;message=e.what();}
    if(error&&!changed&&!desktop.empty())try{Launch(desktop);}catch(...){}
    try{WriteAtomic(root/"switch-result.json",Json{{"id",id},{"message",message},{"error",error},{"at",Timestamp()}}.dump());}catch(...){}
    flock(lock,LOCK_UN);close(lock);return error?1:0;
}
std::string SwitchInHelper(const std::string& id){Validate(id);auto exe=platform::Executable();std::string executable=exe.string(),option="--switch-helper",profile=id;char* argv[]={executable.data(),option.data(),profile.data(),nullptr};posix_spawnattr_t attrs;posix_spawnattr_init(&attrs);posix_spawnattr_setflags(&attrs,POSIX_SPAWN_SETPGROUP);posix_spawnattr_setpgroup(&attrs,0);pid_t pid;int code=posix_spawn(&pid,exe.c_str(),nullptr,&attrs,argv,environ);posix_spawnattr_destroy(&attrs);if(code)throw std::runtime_error("Could not start the switch worker.");int status=0;bool exited=false;for(int i=0;i<900;i++){if(waitpid(pid,&status,WNOHANG)==pid){exited=true;break;}usleep(100000);}if(!exited)throw std::runtime_error("Switch is still finishing. Wait before trying again.");if(WIFEXITED(status)&&WEXITSTATUS(status)==3)throw std::runtime_error("Another switch is already running.");auto result=Json::parse(Read(platform::DataDirectory(exe.parent_path())/"accounts/switch-result.json"));if(result.value("id","")!=id||!WIFEXITED(status))throw std::runtime_error("Could not verify switch result.");if(result.value("error",true)||WEXITSTATUS(status))throw std::runtime_error(result.value("message","Switch failed."));return result.value("message","Account switched.");}
}
