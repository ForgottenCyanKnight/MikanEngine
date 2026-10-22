#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <chrono>
#include <algorithm>
#ifdef _WIN32
#include <windows.h>
#endif

// A CPU-only loading window. Its message loop never calls Vulkan, SDL or ImGui.
// It remains responsive while the render thread imports assets/builds pipelines.
namespace Core {
class ProjectLoadProgress {
    std::atomic<bool> active{false};
    std::mutex mutex;
    std::string status;
    std::thread worker;
#ifdef _WIN32
    static LRESULT CALLBACK Proc(HWND h,UINT m,WPARAM w,LPARAM l){
        if(m==WM_CLOSE)return 0; // Loading cannot safely be cancelled mid-upload.
        return DefWindowProcW(h,m,w,l);
    }
    void Run(){
        const wchar_t* cls=L"MikanProjectLoading";
        WNDCLASSW wc{};wc.lpfnWndProc=Proc;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=cls;wc.hCursor=LoadCursor(nullptr,IDC_ARROW);
        RegisterClassW(&wc);
        const int width=620,height=170;
        HWND window=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW,cls,L"Mikan Engine - Loading project",
            WS_POPUP|WS_CAPTION,(GetSystemMetrics(SM_CXSCREEN)-width)/2,(GetSystemMetrics(SM_CYSCREEN)-height)/2,
            width,height,nullptr,nullptr,wc.hInstance,nullptr);
        if(!window)return;
        ShowWindow(window,SW_SHOWNOACTIVATE);
        unsigned tick=0;
        while(active){
            MSG msg;while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}
            std::string text;{std::lock_guard lock(mutex);text=status;}
            int n=MultiByteToWideChar(CP_UTF8,0,text.c_str(),-1,nullptr,0);std::wstring wide(n,L'\0');
            if(n)MultiByteToWideChar(CP_UTF8,0,text.c_str(),-1,wide.data(),n);
            RECT client;GetClientRect(window,&client);HDC dc=GetDC(window);
            HBRUSH bg=CreateSolidBrush(RGB(25,30,40));FillRect(dc,&client,bg);DeleteObject(bg);
            SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(230,235,245));
            HFONT font=CreateFontW(-20,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Microsoft YaHei UI");
            auto old=SelectObject(dc,font);RECT label{25,20,590,70};DrawTextW(dc,wide.c_str(),-1,&label,DT_LEFT|DT_WORDBREAK);
            RECT bar{25,85,590,104};HBRUSH track=CreateSolidBrush(RGB(48,57,72));FillRect(dc,&bar,track);DeleteObject(track);
            // Indeterminate: stage timings are unknown; never invent a percentage.
            int x=25+int(tick++%95)*5;RECT pulse{x,85,(std::min)(x+90,590),104};
            HBRUSH fill=CreateSolidBrush(RGB(70,155,235));FillRect(dc,&pulse,fill);DeleteObject(fill);
            SelectObject(dc,old);DeleteObject(font);ReleaseDC(window,dc);
            std::this_thread::sleep_for(std::chrono::milliseconds(33));
        }
        DestroyWindow(window);
    }
#endif
public:
    static ProjectLoadProgress& Get(){static ProjectLoadProgress p;return p;}
    ~ProjectLoadProgress(){Finish();}
    void Begin(){
#ifdef _WIN32
        Finish();status="Preparing project...";active=true;worker=std::thread([this]{Run();});
#endif
    }
    void Update(const std::string& text){if(active){std::lock_guard lock(mutex);status=text;}}
    void Finish(){active=false;if(worker.joinable())worker.join();}
};
}
