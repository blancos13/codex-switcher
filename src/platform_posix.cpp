#include "platform.h"
#include "platform_posix.h"
#include <unistd.h>
#include <pwd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <poll.h>
#include <signal.h>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <codecvt>
#include <locale>
#include <fstream>
#include <ctime>
#include <mutex>
#include <openssl/rand.h>
#include <openssl/evp.h>
#include <openssl/crypto.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#else
#include <libsecret/secret.h>
#endif
namespace platform {
static void Fail(const char* text){throw std::runtime_error(text);}
void Erase(void* data,size_t size){if(size)OPENSSL_cleanse(data,size);}
bool RemoveFile(const Path& path){return unlink(path.c_str())==0;}
Path Executable(){
#ifdef __APPLE__
    uint32_t size=0;_NSGetExecutablePath(nullptr,&size);std::vector<char> path(size);if(_NSGetExecutablePath(path.data(),&size))Fail("Could not find executable.");return std::filesystem::canonical(path.data());
#else
    std::vector<char> path(32768);ssize_t n=readlink("/proc/self/exe",path.data(),path.size()-1);if(n<=0)Fail("Could not find executable.");return Path(std::string(path.data(),size_t(n)));
#endif
}
Path DataDirectory(const Path&){
    if(const char* custom=getenv("CODEX_SWITCHER_DATA_HOME")){if(*custom)return std::filesystem::absolute(custom);}
#ifdef __APPLE__
    return accounts::UserHome()/"Library/Application Support/CodexSwitcher";
#else
    if(const char* xdg=getenv("XDG_DATA_HOME")){if(*xdg&&Path(xdg).is_absolute())return Path(xdg)/"codex-switcher";}
    return accounts::UserHome()/".local/share/codex-switcher";
#endif
}
Path AssetDirectory(const Path& root){
#ifdef __APPLE__
    if(root.filename()=="MacOS")return root.parent_path()/"Resources/assets";
#endif
    if(std::filesystem::exists(root/"assets"))return root/"assets";
    return root.parent_path()/"share/codex-switcher/assets";
}
static std::vector<char*> Args(const Path& exe,const std::vector<std::string>& args,std::vector<std::string>& owned){owned.push_back(exe.string());owned.insert(owned.end(),args.begin(),args.end());std::vector<char*> pointers;for(auto& s:owned)pointers.push_back(s.data());pointers.push_back(nullptr);return pointers;}
void Spawn(const Path& executable,const std::vector<std::string>& args){
    std::vector<std::string> owned;auto argv=Args(executable,args,owned);pid_t child=fork();if(child<0)Fail("Could not launch application.");
    if(!child){pid_t grandchild=fork();if(grandchild<0)_exit(127);if(grandchild>0)_exit(0);setsid();int fd=open("/dev/null",O_RDWR);if(fd>=0){dup2(fd,0);dup2(fd,1);dup2(fd,2);if(fd>2)close(fd);}execv(executable.c_str(),argv.data());_exit(127);}
    int status=0;while(waitpid(child,&status,0)<0&&errno==EINTR){}if(!WIFEXITED(status)||WEXITSTATUS(status))Fail("Could not launch application.");
}
std::string RunCapture(const Path& exe,const std::vector<std::string>& args,int timeout){
    int pipes[2];if(pipe(pipes))Fail("Could not create process pipe.");std::vector<std::string> owned;auto argv=Args(exe,args,owned);
    pid_t child=fork();if(child<0){close(pipes[0]);close(pipes[1]);Fail("Could not start process.");}
    if(!child){setpgid(0,0);close(pipes[0]);dup2(pipes[1],1);close(pipes[1]);int fd=open("/dev/null",O_RDWR);if(fd>=0){dup2(fd,0);dup2(fd,2);close(fd);}execv(exe.c_str(),argv.data());_exit(127);}
    setpgid(child,child);close(pipes[1]);fcntl(pipes[0],F_SETFL,O_NONBLOCK);std::string output;int elapsed=0,status=0;bool exited=false;
    while(elapsed<timeout*1000){pollfd p{pipes[0],POLLIN,0};poll(&p,1,50);elapsed+=50;char data[4096];ssize_t n;while((n=read(pipes[0],data,sizeof(data)))>0){output.append(data,size_t(n));if(output.size()>65536)break;}if(output.size()>65536)break;if(waitpid(child,&status,WNOHANG)==child){exited=true;break;}}
    if(!exited){kill(-child,SIGKILL);while(waitpid(child,&status,0)<0&&errno==EINTR){}}close(pipes[0]);
    if(!exited||!WIFEXITED(status)||WEXITSTATUS(status)!=0)Fail("Codex version lookup failed or timed out.");while(!output.empty()&&(output.back()=='\n'||output.back()=='\r'))output.pop_back();return output;
}
void OpenUrl(const std::string& url){
#ifdef __APPLE__
    Spawn("/usr/bin/open",{url});
#else
    Spawn("/usr/bin/xdg-open",{url});
#endif
}
void OpenFolder(const Path& p){OpenUrl(p.string());}
Path FindDesktop(){
#ifdef __APPLE__
    for(const auto& root:{Path("/Applications/Codex.app"),accounts::UserHome()/"Applications/Codex.app",Path("/Applications/ChatGPT.app")}){
        if(!std::filesystem::exists(root/"Contents/Info.plist"))continue;
        // Verify the bundle is OpenAI-signed before exposing it as a restart target.
        CFURLRef url=CFURLCreateFromFileSystemRepresentation(nullptr,reinterpret_cast<const UInt8*>(root.c_str()),root.string().size(),true);
        SecStaticCodeRef code=nullptr;SecRequirementRef requirement=nullptr;bool valid=false;
        if(url&&SecStaticCodeCreateWithPath(url,kSecCSDefaultFlags,&code)==errSecSuccess&&SecRequirementCreateWithString(CFSTR("anchor apple generic and certificate leaf[subject.OU] = \"2DC432GLL2\""),kSecCSDefaultFlags,&requirement)==errSecSuccess)valid=SecStaticCodeCheckValidity(code,kSecCSCheckAllArchitectures,requirement)==errSecSuccess;
        if(requirement)CFRelease(requirement);if(code)CFRelease(code);if(url)CFRelease(url);if(valid)return root;
    }
#else
    if(const char* path=getenv("CODEX_DESKTOP_PATH")){if(*path&&Path(path).is_absolute()&&access(path,X_OK)==0)return std::filesystem::canonical(path);}
    for(const auto& root:{Path("/opt/Codex/codex"),Path("/opt/codex/codex"),Path("/usr/lib/codex/codex"),accounts::UserHome()/".local/share/Codex/codex"})if(access(root.c_str(),X_OK)==0)return std::filesystem::canonical(root);
#endif
    return {};
}
std::string InstallationInfo(){
    std::string info;auto desktop=FindDesktop();info=desktop.empty()?"Codex desktop: not found":"Codex desktop: "+desktop.string();
#ifdef __APPLE__
    if(!desktop.empty())try{info+="\nDesktop version: "+RunCapture("/usr/libexec/PlistBuddy",{"-c","Print :CFBundleShortVersionString",(desktop/"Contents/Info.plist").string()});}catch(...){}
#endif
    try{auto cli=accounts::FindCli();info+="\nCLI: "+cli.string()+"\n"+RunCapture(cli,{"--version"});}catch(const std::exception& e){info+="\n"+std::string(e.what());}return info;
}
}
namespace accounts {
std::wstring Wide(const std::string& s){return std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>>{}.from_bytes(s);}
std::string Utf8(const std::wstring& s){return std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>>{}.to_bytes(s);}
Path UserHome(){if(const char* home=getenv("HOME")){if(*home&&Path(home).is_absolute())return home;}auto* user=getpwuid(getuid());if(!user)throw std::runtime_error("User home is unavailable.");return user->pw_dir;}
Path ActiveAuth(){if(const char* home=getenv("CODEX_HOME")){if(*home)return std::filesystem::absolute(home)/"auth.json";}return UserHome()/".codex/auth.json";}
std::string Guid(){unsigned char bytes[16];if(RAND_bytes(bytes,16)!=1)throw std::runtime_error("Random generator unavailable.");std::string s;for(auto b:bytes){s+="0123456789abcdef"[b>>4];s+="0123456789abcdef"[b&15];}return s;}
std::string Timestamp(){std::time_t now=std::time(nullptr);std::tm utc{};gmtime_r(&now,&utc);char buffer[32];std::strftime(buffer,sizeof(buffer),"%Y-%m-%dT%H:%M:%SZ",&utc);return buffer;}
std::string Read(const Path& path){std::ifstream f(path,std::ios::binary|std::ios::ate);if(!f)throw std::runtime_error("Could not read local file.");auto n=f.tellg();if(n<0||n>8*1024*1024)throw std::runtime_error("Invalid local file size.");std::string s(size_t(n),'\0');f.seekg(0);if(!f.read(s.data(),n))throw std::runtime_error("Could not read local file.");return s;}
void WriteAtomic(const Path& path,const std::string& data){
    std::filesystem::create_directories(path.parent_path());auto temp=path;temp+="."+Guid()+".tmp";int fd=open(temp.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);if(fd<0)throw std::runtime_error("Could not create private local file.");
    size_t offset=0;while(offset<data.size()){ssize_t n=write(fd,data.data()+offset,data.size()-offset);if(n<0&&errno==EINTR)continue;if(n<=0){close(fd);unlink(temp.c_str());throw std::runtime_error("Could not write local file.");}offset+=size_t(n);}
    bool ok=fsync(fd)==0;close(fd);if(!ok||rename(temp.c_str(),path.c_str())){unlink(temp.c_str());throw std::runtime_error("Could not save local file.");}int dir=open(path.parent_path().c_str(),O_RDONLY|O_DIRECTORY);if(dir>=0){fsync(dir);close(dir);}
}
static std::mutex vaultMutex;
static std::string Key(bool create){
    std::lock_guard<std::mutex> guard(vaultMutex);std::string hex;
#ifdef __APPLE__
    const void* keys[]={kSecClass,kSecAttrService,kSecAttrAccount,kSecReturnData,kSecMatchLimit};
    const void* values[]={kSecClassGenericPassword,CFSTR("CodexSwitcher"),CFSTR("profile-encryption-v1"),kCFBooleanTrue,kSecMatchLimitOne};
    CFDictionaryRef query=CFDictionaryCreate(nullptr,keys,values,5,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);CFTypeRef result=nullptr;OSStatus status=SecItemCopyMatching(query,&result);CFRelease(query);
    if(status==errSecSuccess){auto data=static_cast<CFDataRef>(result);hex.assign(reinterpret_cast<const char*>(CFDataGetBytePtr(data)),CFDataGetLength(data));CFRelease(result);}
    else if(status!=errSecItemNotFound)throw std::runtime_error("Unlock your macOS login Keychain and retry.");
#else
    static const SecretSchema schema={"org.codex-switcher.EncryptionKey",SECRET_SCHEMA_NONE,{{"key",SECRET_SCHEMA_ATTRIBUTE_STRING},{nullptr,SECRET_SCHEMA_ATTRIBUTE_STRING}}};GError* error=nullptr;
    gchar* value=secret_password_lookup_sync(&schema,nullptr,&error,"key","profile-encryption-v1",nullptr);
    if(error){g_error_free(error);throw std::runtime_error("Unlock a Secret Service keyring (GNOME Keyring or KWallet) and retry.");}if(value){hex=value;secret_password_free(value);}
#endif
    if(hex.empty()){
        if(!create)throw std::runtime_error("Profile encryption key is missing. Sign in again on this machine.");hex=Guid()+Guid();
#ifdef __APPLE__
        CFDataRef data=CFDataCreate(nullptr,reinterpret_cast<const UInt8*>(hex.data()),hex.size());
        const void* addKeys[]={kSecClass,kSecAttrService,kSecAttrAccount,kSecValueData};const void* addValues[]={kSecClassGenericPassword,CFSTR("CodexSwitcher"),CFSTR("profile-encryption-v1"),data};CFDictionaryRef add=CFDictionaryCreate(nullptr,addKeys,addValues,4,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);OSStatus saved=SecItemAdd(add,nullptr);CFRelease(add);CFRelease(data);if(saved!=errSecSuccess)throw std::runtime_error("Could not save the profile encryption key in Keychain.");
#else
        GError* saveError=nullptr;gboolean saved=secret_password_store_sync(&schema,SECRET_COLLECTION_DEFAULT,"Codex Switcher encryption key",hex.c_str(),nullptr,&saveError,"key","profile-encryption-v1",nullptr);if(saveError)g_error_free(saveError);if(!saved)throw std::runtime_error("Could not save the profile encryption key in Secret Service.");
#endif
    }
    if(hex.size()!=64)throw std::runtime_error("Invalid profile encryption key.");std::string raw(32,'\0');auto nibble=[](char c)->int{if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;return -1;};for(size_t i=0;i<32;i++){int a=nibble(hex[i*2]),b=nibble(hex[i*2+1]);if(a<0||b<0)throw std::runtime_error("Invalid profile encryption key.");raw[i]=char(a*16+b);}platform::Erase(hex.data(),hex.size());return raw;
}
static std::string Crypt(const std::string& data,bool encrypt){
    const std::string magic="CSAES1";if(!encrypt&&(data.size()<34||data.substr(0,6)!=magic))throw std::runtime_error("This profile belongs to another OS or uses an unsupported format.");
    auto key=Key(encrypt);unsigned char iv[12],tag[16];if(encrypt){if(RAND_bytes(iv,12)!=1)throw std::runtime_error("Random generator unavailable.");}else{memcpy(iv,data.data()+6,12);memcpy(tag,data.data()+18,16);}
    EVP_CIPHER_CTX* ctx=EVP_CIPHER_CTX_new();if(!ctx){platform::Erase(key.data(),key.size());throw std::runtime_error("Encryption is unavailable.");}
    std::string output(data.size()+32,'\0');int n=0,total=0;bool ok;
    if(encrypt){ok=EVP_EncryptInit_ex(ctx,EVP_aes_256_gcm(),nullptr,reinterpret_cast<const unsigned char*>(key.data()),iv)==1&&EVP_EncryptUpdate(ctx,reinterpret_cast<unsigned char*>(output.data()),&n,reinterpret_cast<const unsigned char*>(data.data()),int(data.size()))==1;total=n;ok=ok&&EVP_EncryptFinal_ex(ctx,reinterpret_cast<unsigned char*>(output.data())+total,&n)==1;total+=n;ok=ok&&EVP_CIPHER_CTX_ctrl(ctx,EVP_CTRL_GCM_GET_TAG,16,tag)==1;}
    else{ok=EVP_DecryptInit_ex(ctx,EVP_aes_256_gcm(),nullptr,reinterpret_cast<const unsigned char*>(key.data()),iv)==1&&EVP_DecryptUpdate(ctx,reinterpret_cast<unsigned char*>(output.data()),&n,reinterpret_cast<const unsigned char*>(data.data()+34),int(data.size()-34))==1;total=n;ok=ok&&EVP_CIPHER_CTX_ctrl(ctx,EVP_CTRL_GCM_SET_TAG,16,tag)==1&&EVP_DecryptFinal_ex(ctx,reinterpret_cast<unsigned char*>(output.data())+total,&n)==1;total+=n;}
    EVP_CIPHER_CTX_free(ctx);platform::Erase(key.data(),key.size());if(!ok){platform::Erase(output.data(),output.size());throw std::runtime_error("Could not unlock the saved profile.");}output.resize(size_t(total));if(encrypt)return magic+std::string(reinterpret_cast<char*>(iv),12)+std::string(reinterpret_cast<char*>(tag),16)+output;return output;
}
std::string Protect(const std::string& s){return Crypt(s,true);}std::string Unprotect(const std::string& s){return Crypt(s,false);}
}
