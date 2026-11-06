#include "common.hpp"
#include "sessions.hpp"
#include <shellapi.h>

namespace {
HWND edit=nullptr;std::wstring output;bool slowUia=false;
LRESULT CALLBACK window_proc(HWND window,UINT message,WPARAM w,LPARAM l) {
    switch(message) {
    case WM_GETOBJECT:
        if(slowUia)Sleep(8000); // Simulate a blocked accessibility provider.
        break;
    case WM_CREATE:
        edit=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_MULTILINE|ES_AUTOVSCROLL|WS_VSCROLL,20,20,580,200,window,reinterpret_cast<HMENU>(101),nullptr,nullptr);
        CreateWindowW(L"BUTTON",L"Save UTF-8",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_DEFPUSHBUTTON,20,235,160,40,window,reinterpret_cast<HMENU>(102),nullptr,nullptr);
        SetFocus(edit);return 0;
    case WM_COMMAND:
        if(LOWORD(w)==102) {
            int count=GetWindowTextLengthW(edit);std::wstring text(count+1,L'\0');GetWindowTextW(edit,text.data(),count+1);text.resize(count);
            auto bytes=ra::utf8(text);ra::Handle file(CreateFileW(output.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr));DWORD done=0;
            bool ok=file.valid() && WriteFile(file.get(),bytes.data(),static_cast<DWORD>(bytes.size()),&done,nullptr) && done==bytes.size();
            SetWindowTextW(window,ok?L"RemoteAgent Test - Saved":L"RemoteAgent Test - Save failed");return 0;
        }break;
    case WM_DESTROY:PostQuitMessage(0);return 0;
    }
    return DefWindowProcW(window,message,w,l);
}
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR,int show) {
    ra::enable_dpi_awareness();int count=0;auto args=CommandLineToArgvW(GetCommandLineW(),&count);
    if(!args || (count!=2 && count!=3)){if(args)LocalFree(args);return 2;}output=args[1];
    bool displayOff=count==3 && std::wstring(args[2])==L"--lock-display-off";
    bool lock=count==3 && (std::wstring(args[2])==L"--lock" || displayOff);
    slowUia=count==3 && std::wstring(args[2])==L"--slow-uia";LocalFree(args);
    if(lock){SetLastError(0);BOOL ok=LockWorkStation();DWORD error=GetLastError();
        if(ok && displayOff){Sleep(1000);DWORD_PTR ignored=0;SendMessageTimeoutW(HWND_BROADCAST,WM_SYSCOMMAND,SC_MONITORPOWER,2,SMTO_ABORTIFHUNG,500,&ignored);}
        auto text=ra::json({{"lock_requested",ok!=FALSE},{"win32",error},{"session_id",ra::process_session()}}).dump();
        ra::Handle file(CreateFileW(output.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr));DWORD done=0;
        if(file.valid())WriteFile(file.get(),text.data(),static_cast<DWORD>(text.size()),&done,nullptr);Sleep(5000);return ok?0:1;}
    WNDCLASSW cls{};cls.lpfnWndProc=window_proc;cls.hInstance=instance;cls.lpszClassName=L"RemoteAgentTest";cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);cls.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);
    if(!RegisterClassW(&cls))return 3;
    const wchar_t* title=ra::process_privileges().high()?L"RemoteAgent Test - Elevated":L"RemoteAgent Test";
    HWND window=CreateWindowW(cls.lpszClassName,title,WS_OVERLAPPEDWINDOW,100,100,650,340,nullptr,nullptr,instance,nullptr);
    if(!window)return 4;ShowWindow(window,show);UpdateWindow(window);MSG msg{};
    while(GetMessageW(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}return 0;
}
