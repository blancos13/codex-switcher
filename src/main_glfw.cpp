#include "studio.h"
#include "platform.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#ifdef __APPLE__
#define GLFW_INCLUDE_GLCOREARB
#endif
#include <GLFW/glfw3.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <iostream>
int main(int argc,char** argv){
    signal(SIGPIPE,SIG_IGN);
    try{
        if(argc==3&&std::string(argv[1])=="--switch-helper")return accounts::SwitchHelper(argv[2]);
        if(argc==2&&std::string(argv[1])=="--probe"){std::atomic_bool cancel=false;return accounts::Probe(cancel)?0:1;}
        const auto root=platform::Executable().parent_path(),data=platform::DataDirectory(root);std::filesystem::create_directories(data);
        int lock=open((data/"app.lock").c_str(),O_RDWR|O_CREAT|O_CLOEXEC,0600);if(lock<0||flock(lock,LOCK_EX|LOCK_NB)){if(lock>=0)close(lock);std::cerr<<"Codex Switcher is already running.\n";return 1;}
        if(!glfwInit()){close(lock);throw std::runtime_error("Could not initialize GLFW. A desktop session is required.");}
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR,3);
#ifdef __APPLE__
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR,2);glfwWindowHint(GLFW_OPENGL_PROFILE,GLFW_OPENGL_CORE_PROFILE);glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT,GL_TRUE);const char* glsl="#version 150";
#else
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR,0);const char* glsl="#version 130";
#endif
        GLFWwindow* window=glfwCreateWindow(1000,600,"Codex Switcher",nullptr,nullptr);if(!window){glfwTerminate();close(lock);throw std::runtime_error("Could not create OpenGL window.");}
        glfwMakeContextCurrent(window);glfwSwapInterval(1);glfwSetWindowSizeLimits(window,560,480,GLFW_DONT_CARE,GLFW_DONT_CARE);
        IMGUI_CHECKVERSION();ImGui::CreateContext();auto& io=ImGui::GetIO();io.ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard;io.IniFilename=nullptr;
        {
            Studio app(root);app.InitFonts();ImGui_ImplGlfw_InitForOpenGL(window,true);ImGui_ImplOpenGL3_Init(glsl);
            while(!glfwWindowShouldClose(window)&&!app.exitRequested){glfwPollEvents();app.Tick();if(glfwGetWindowAttrib(window,GLFW_ICONIFIED)){glfwWaitEventsTimeout(.1);continue;}
                ImGui_ImplOpenGL3_NewFrame();ImGui_ImplGlfw_NewFrame();ImGui::NewFrame();app.Draw();ImGui::Render();int width,height;glfwGetFramebufferSize(window,&width,&height);glViewport(0,0,width,height);glClearColor(.06f,.06f,.06f,1);glClear(GL_COLOR_BUFFER_BIT);ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());glfwSwapBuffers(window);
            }
            app.Save();ImGui_ImplOpenGL3_Shutdown();ImGui_ImplGlfw_Shutdown();
        }
        ImGui::DestroyContext();glfwDestroyWindow(window);glfwTerminate();flock(lock,LOCK_UN);close(lock);return 0;
    }catch(const std::exception& e){std::cerr<<"Codex Switcher: "<<e.what()<<'\n';return 1;}
}
