#include "accounts.h"
#include "platform.h"
#include "platform_posix.h"
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <chrono>
#include <algorithm>
#include <cmath>
extern char** environ;
namespace accounts {
static void Require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
Path FindCli(){
    std::vector<Path> candidates;auto desktop=platform::FindDesktop();
#ifdef __APPLE__
    if(!desktop.empty())for(const auto& relative:{"Contents/Resources/codex","Contents/Resources/bin/codex","Contents/Resources/app.asar.unpacked/codex"})candidates.push_back(desktop/relative);
#endif
    if(const char* overridePath=getenv("CODEX_CLI_PATH")){if(*overridePath&&Path(overridePath).is_absolute())candidates.insert(candidates.begin(),overridePath);}
    candidates.push_back(UserHome()/".local/bin/codex");candidates.push_back("/opt/homebrew/bin/codex");candidates.push_back("/usr/local/bin/codex");candidates.push_back("/usr/bin/codex");
    if(const char* env=getenv("PATH")){std::string paths=env;size_t from=0;while(from<=paths.size()){auto end=paths.find(':',from);auto dir=paths.substr(from,end==std::string::npos?std::string::npos:end-from);if(!dir.empty()&&Path(dir).is_absolute())candidates.push_back(Path(dir)/"codex");if(end==std::string::npos)break;from=end+1;}}
    for(auto& path:candidates){std::error_code ec;auto real=std::filesystem::canonical(path,ec);if(ec||access(real.c_str(),X_OK)!=0)continue;struct stat st{};if(stat(real.c_str(),&st)||!S_ISREG(st.st_mode)||(st.st_mode&(S_IWGRP|S_IWOTH))||(st.st_uid!=0&&st.st_uid!=getuid()))continue;return real;}
    throw std::runtime_error("Codex CLI was not found. Install the official CLI or set CODEX_CLI_PATH to its absolute path.");
}
class Session {
    Path home;pid_t pid=-1;int input=-1,output=-1;std::atomic_bool& canceled;std::string buffer;int nextId=1;std::vector<Json> notifications;
    void Check(){if(canceled.load())throw OperationCanceled();}
    void Cleanup(){
        if(input>=0){close(input);input=-1;}
        if(pid>0){kill(-pid,SIGTERM);int status;bool done=false;for(int i=0;i<30;i++){if(waitpid(pid,&status,WNOHANG)==pid){done=true;break;}usleep(10000);}if(!done){kill(-pid,SIGKILL);while(waitpid(pid,&status,0)<0&&errno==EINTR){}}else kill(-pid,SIGKILL);pid=-1;}
        if(output>=0){close(output);output=-1;}
        if(!home.empty()){std::error_code ec;std::filesystem::remove_all(home,ec);}
    }
    void Send(const Json& json){
        auto data=json.dump()+"\n";size_t sent=0;auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
        while(sent<data.size()){Check();Require(std::chrono::steady_clock::now()<deadline,"Codex communication timed out.");pollfd p{input,POLLOUT,0};if(poll(&p,1,50)<0&&errno!=EINTR)throw std::runtime_error("Codex communication failed.");ssize_t n=write(input,data.data()+sent,data.size()-sent);if(n<0&&(errno==EAGAIN||errno==EINTR))continue;Require(n>0,"Codex communication failed.");sent+=size_t(n);}
    }
    Json ReadMessage(std::chrono::steady_clock::time_point deadline){
        while(true){Check();Require(std::chrono::steady_clock::now()<deadline,"Codex request timed out. Retry later.");auto end=buffer.find('\n');if(end!=std::string::npos){auto line=buffer.substr(0,end);buffer.erase(0,end+1);auto j=Json::parse(line,nullptr,false);if(j.is_object())return j;continue;}
            pollfd p{output,POLLIN,0};int result=poll(&p,1,50);if(result<0&&errno==EINTR)continue;Require(result>=0,"Codex communication failed.");if(!result)continue;char bytes[4096];ssize_t n=read(output,bytes,sizeof(bytes));if(n<0&&(errno==EINTR||errno==EAGAIN))continue;Require(n>0,"Codex App Server stopped.");buffer.append(bytes,size_t(n));Require(buffer.size()<4*1024*1024,"Codex response exceeded size limit.");
        }
    }
public:
    Session(Path base,std::atomic_bool& flag,const std::string& auth):canceled(flag){
        int readPipe[2]={-1,-1},writePipe[2]={-1,-1};
        try {
            auto exe=FindCli();Check();std::filesystem::create_directories(base);home=base/Guid();Require(mkdir(home.c_str(),0700)==0,"Could not create private login directory.");
            WriteAtomic(home/"config.toml","cli_auth_credentials_store = \"file\"\n");if(!auth.empty())WriteAtomic(home/"auth.json",auth);
            Require(pipe(readPipe)==0&&pipe(writePipe)==0,"Could not create Codex pipes.");for(int fd:{readPipe[0],readPipe[1],writePipe[0],writePipe[1]})fcntl(fd,F_SETFD,FD_CLOEXEC);
            std::vector<std::string> environment;for(char** p=environ;*p;++p)if(std::strncmp(*p,"CODEX_HOME=",11)!=0)environment.emplace_back(*p);environment.push_back("CODEX_HOME="+home.string());std::vector<char*> env;for(auto& s:environment)env.push_back(s.data());env.push_back(nullptr);
            std::string exeText=exe.string(),command="app-server";char* args[]={exeText.data(),command.data(),nullptr};
            posix_spawn_file_actions_t actions;posix_spawn_file_actions_init(&actions);
            posix_spawn_file_actions_adddup2(&actions,readPipe[0],0);posix_spawn_file_actions_adddup2(&actions,writePipe[1],1);
            posix_spawn_file_actions_addopen(&actions,2,"/dev/null",O_WRONLY,0);
            for(int fd:{readPipe[0],readPipe[1],writePipe[0],writePipe[1]})posix_spawn_file_actions_addclose(&actions,fd);
            posix_spawnattr_t attrs;posix_spawnattr_init(&attrs);posix_spawnattr_setflags(&attrs,POSIX_SPAWN_SETPGROUP);posix_spawnattr_setpgroup(&attrs,0);
            int code=posix_spawn(&pid,exe.c_str(),&actions,&attrs,args,env.data());posix_spawnattr_destroy(&attrs);posix_spawn_file_actions_destroy(&actions);Require(code==0,"Could not launch Codex App Server.");
            input=readPipe[1];readPipe[1]=-1;output=writePipe[0];writePipe[0]=-1;close(readPipe[0]);readPipe[0]=-1;close(writePipe[1]);writePipe[1]=-1;fcntl(input,F_SETFL,O_NONBLOCK);fcntl(output,F_SETFL,O_NONBLOCK);
            Request("initialize",{{"clientInfo",{{"name","codex_switcher"},{"title","Codex Switcher"},{"version","0.2.0"}}}});Send({{"method","initialized"},{"params",Json::object()}});
        }catch(...){for(int fd:{readPipe[0],readPipe[1],writePipe[0],writePipe[1]})if(fd>=0)close(fd);Cleanup();throw;}
    }
    ~Session(){Cleanup();}
    Json Request(const std::string& method,const Json& params=Json::object()){
        int id=nextId++;Send({{"id",id},{"method",method},{"params",params}});auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(40);
        while(true){auto j=ReadMessage(deadline);if(j.contains("id")&&j["id"]==id){Require(!j.contains("error"),"Codex rejected the request. Sign in again or retry later.");return j.at("result");}if(j.contains("method"))notifications.push_back(j);}
    }
    Json WaitLogin(){auto deadline=std::chrono::steady_clock::now()+std::chrono::minutes(10);while(true){for(auto& j:notifications)if(j.value("method","")=="account/login/completed")return j.at("params");notifications.clear();auto j=ReadMessage(deadline);if(j.value("method","")=="account/login/completed")return j.at("params");}}
    std::string Auth(){return std::filesystem::exists(home/"auth.json")?Read(home/"auth.json"):"";}
};
#include "codex_protocol.inc"
}
