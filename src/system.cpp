#include "system.hpp"
#include "bridge.hpp"
#include "desktop.hpp"
#include "display.hpp"
#include <taskschd.h>
#include <uiautomation.h>
#include <wtsapi32.h>
#include <tlhelp32.h>
#include <algorithm>

namespace ra {
namespace {
void enable_privilege(const wchar_t* name) {
    HANDLE raw=nullptr;check_win(OpenProcessToken(GetCurrentProcess(),TOKEN_ADJUST_PRIVILEGES|TOKEN_QUERY,&raw),"Open privilege token");Handle token(raw);
    TOKEN_PRIVILEGES p{};p.PrivilegeCount=1;check_win(LookupPrivilegeValueW(nullptr,name,&p.Privileges[0].Luid),"Lookup privilege");
    p.Privileges[0].Attributes=SE_PRIVILEGE_ENABLED;SetLastError(ERROR_SUCCESS);
    check_win(AdjustTokenPrivileges(token.get(),FALSE,&p,0,nullptr,nullptr),"Enable privilege");
    check_win(GetLastError()==ERROR_SUCCESS,"Privilege unavailable");
}
std::string object_name(HANDLE h) {
    wchar_t name[512]{};DWORD n=0;check_win(GetUserObjectInformationW(h,UOI_NAME,name,sizeof(name),&n),"Query desktop name");return utf8(name);
}
struct InputDesktop {
    HDESK value=nullptr;
    explicit InputDesktop(bool attach) {
        DWORD access=GENERIC_ALL;
        value=OpenInputDesktop(0,FALSE,access);
        check_win(value!=nullptr,"Open SYSTEM input desktop");
        if(attach)check_win(SetThreadDesktop(value),"Attach SYSTEM input desktop");
    }
    // An attached desktop remains owned by the thread until process exit.
    ~InputDesktop(){if(value)CloseDesktop(value);}
};
std::string input_name(){InputDesktop input(false);return object_name(input.value);}
std::wstring process_image(DWORD pid) {
    Handle p(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid));if(!p.valid())return {};
    wchar_t path[32768]{};DWORD n=32768;if(!QueryFullProcessImageNameW(p.get(),0,path,&n))return {};
    std::wstring s(path,n);return s.substr(s.find_last_of(L"\\/")+1);
}
struct PasswordField {
    ComPtr<IUIAutomation> api;
    ComPtr<IUIAutomationElement> field;
    ComPtr<IUIAutomationElement> priorErrorOk;
    bool priorIncorrect=false;
    int count=0;
    int nodes=0;
    json candidates=json::array();
    json tree=json::array();
    bool curtain=false,accountSelected=false;
    PasswordField(const json& account=json::object()) {
        check_hr(CoCreateInstance(CLSID_CUIAutomation,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&api)),"Create logon UIA");
        HWND window=FindWindowW(L"LogonUI Logon Window",nullptr);
        ComPtr<IUIAutomationElement> root;check_hr(api->ElementFromHandle(window?window:GetDesktopWindow(),&root),"Get logon UIA root");
        ComPtr<IUIAutomationCondition> condition;check_hr(api->CreateTrueCondition(&condition),"Logon UIA condition");
        ComPtr<IUIAutomationElementArray> elements;check_hr(root->FindAll(TreeScope_Descendants,condition.Get(),&elements),"Find logon fields");
        int n=0;check_hr(elements->get_Length(&n),"Logon field count");nodes=n;
        if(n>1000)throw Error("UNSUPPORTED_LOGON_UI","Logon UI tree is too large");
        int selectedTitles=0,namedTitles=0;
        ComPtr<IUIAutomationTreeWalker> walker;check_hr(api->get_RawViewWalker(&walker),"Logon UIA walker");
        for(int i=0;i<n;++i) {
            ComPtr<IUIAutomationElement> e;elements->GetElement(i,&e);if(!e)continue;
            BOOL password=FALSE,enabled=FALSE,offscreen=TRUE;CONTROLTYPEID type=0;int pid=0;
            e->get_CurrentIsPassword(&password);e->get_CurrentIsEnabled(&enabled);e->get_CurrentIsOffscreen(&offscreen);e->get_CurrentControlType(&type);e->get_CurrentProcessId(&pid);
            BSTR rawName=nullptr;e->get_CurrentName(&rawName);std::wstring nodeName=rawName?rawName:L"";SysFreeString(rawName);
            BSTR rawId=nullptr;e->get_CurrentAutomationId(&rawId);std::wstring nodeId=rawId?rawId:L"";SysFreeString(rawId);
            if(n<80)tree.push_back({{"name",password?std::string("<password>"):utf8(nodeName)},{"automation_id",utf8(nodeId)},{"control_type",type},{"enabled",enabled!=FALSE},{"offscreen",offscreen!=FALSE}});
            if(enabled && !offscreen && (nodeId==L"LockScrollViewer" || nodeId==L"TimeText"))curtain=true;
            if(enabled && !offscreen && !account.empty()) {
                bool matches=false;for(const auto& label:account.value("ui_labels",json::array()))if(_wcsicmp(nodeName.c_str(),wide(label.get<std::string>()).c_str())==0)matches=true;
                if(matches && type==UIA_GroupControlTypeId) {
                    ComPtr<IUIAutomationElement> parent;BSTR id=nullptr;
                    if(SUCCEEDED(walker->GetParentElement(e.Get(),&parent)) && parent && SUCCEEDED(parent->get_CurrentAutomationId(&id))) {
                        if(std::wstring(id?id:L"")==L"UserControlPageRoot")++selectedTitles;SysFreeString(id);
                    }
                }
                if(matches && type==UIA_TextControlTypeId && (nodeId==L"UserTileName" || nodeId==L"UserName" || nodeId==L"UserNameTextBlock"))++namedTitles;
            }
            if(nodeName.find(L"密码不正确")!=std::wstring::npos || nodeName.find(L"password is incorrect")!=std::wstring::npos)priorIncorrect=true;
            if(enabled && !offscreen && type==UIA_ButtonControlTypeId && (nodeName==L"确定" || nodeName==L"OK"))priorErrorOk=e;
            if(password)candidates.push_back({{"control_type",type},{"enabled",enabled!=FALSE},{"offscreen",offscreen!=FALSE},{"exe",utf8(process_image(pid))}});
            if(!password || !enabled || offscreen || type!=UIA_EditControlTypeId || _wcsicmp(process_image(pid).c_str(),L"LogonUI.exe")!=0)continue;
            BSTR name=nullptr,id=nullptr;e->get_CurrentName(&name);e->get_CurrentAutomationId(&id);
            std::wstring label=name?name:L"",automation=id?id:L"";SysFreeString(name);SysFreeString(id);
            std::transform(label.begin(),label.end(),label.begin(),::towlower);
            std::transform(automation.begin(),automation.end(),automation.begin(),::towlower);
            // PIN and other providers can also expose IsPassword. Accept only an
            // identified password field, never an arbitrary focused control.
            if(label.find(L"password")==std::wstring::npos && label.find(L"密码")==std::wstring::npos && automation.find(L"password")==std::wstring::npos)continue;
            field=e;++count;
        }
        accountSelected=selectedTitles==1 || (selectedTitles==0 && namedTitles==1);
    }
};
void send_key(WORD key) {
    INPUT events[2]{};events[0].type=events[1].type=INPUT_KEYBOARD;events[0].ki.wVk=events[1].ki.wVk=key;events[1].ki.dwFlags=KEYEVENTF_KEYUP;
    if(SendInput(2,events,sizeof(INPUT))!=2)throw Error("INPUT_REJECTED","Secure desktop key was not accepted",{{"outcome_unknown",true}});
}
void type_password(const std::wstring& password) {
    std::vector<INPUT> events;events.reserve(password.size()*2);
    for(auto c:password){INPUT a{};a.type=INPUT_KEYBOARD;a.ki.wScan=c;a.ki.dwFlags=KEYEVENTF_UNICODE;INPUT b=a;b.ki.dwFlags|=KEYEVENTF_KEYUP;events.push_back(a);events.push_back(b);}
    UINT accepted=SendInput(static_cast<UINT>(events.size()),events.data(),sizeof(INPUT));
    SecureZeroMemory(events.data(),events.size()*sizeof(INPUT));
    if(accepted!=password.size()*2)throw Error("INPUT_REJECTED","Password input was not fully accepted; no automatic retry",{{"outcome_unknown",true}});
}
json display_info() {
    json displays=json::array();DISPLAY_DEVICEW device{};device.cb=sizeof(device);
    for(DWORD i=0;EnumDisplayDevicesW(nullptr,i,&device,0);++i) {
        if(device.StateFlags&DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)displays.push_back({{"name",utf8(device.DeviceName)},{"description",utf8(device.DeviceString)},{"primary",(device.StateFlags&DISPLAY_DEVICE_PRIMARY_DEVICE)!=0}});
        device={};device.cb=sizeof(device);
    }
    return {{"width",GetSystemMetrics(SM_CXVIRTUALSCREEN)},{"height",GetSystemMetrics(SM_CYVIRTUALSCREEN)},{"monitor_count",GetSystemMetrics(SM_CMONITORS)},{"adapters",displays}};
}
json session_lock_state(){return session_state(process_session());}
json desktop_inventory() {
    json list=json::array();
    EnumDesktopsW(GetProcessWindowStation(),[](LPWSTR desktop,LPARAM data)->BOOL {
        auto& out=*reinterpret_cast<json*>(data);json entry={{"name",utf8(desktop)},{"windows",json::array()}};
        HDESK h=OpenDesktopW(desktop,0,FALSE,DESKTOP_ENUMERATE|DESKTOP_READOBJECTS);
        if(h){EnumDesktopWindows(h,[](HWND w,LPARAM p)->BOOL {
            if(!IsWindowVisible(w))return TRUE;auto& windows=*reinterpret_cast<json*>(p);wchar_t title[256]{},cls[256]{};DWORD pid=0;
            GetWindowTextW(w,title,256);GetClassNameW(w,cls,256);GetWindowThreadProcessId(w,&pid);
            windows.push_back({{"title",utf8(title)},{"class",utf8(cls)},{"pid",pid},{"exe",utf8(process_image(pid))}});return windows.size()<40;
        },reinterpret_cast<LPARAM>(&entry["windows"]));CloseDesktop(h);}
        out.push_back(entry);return TRUE;
    },reinterpret_cast<LPARAM>(&list));return list;
}
}
void erase_password(json& request) {
    if(request.is_object() && request.contains("params") && request["params"].is_object()) {
        auto& p=request["params"];
        if(p.contains("password") && p["password"].is_string()) {auto& s=p["password"].get_ref<std::string&>();if(!s.empty())SecureZeroMemory(s.data(),s.size());p.erase("password");}
    }
}
Session select_unlock_session(const json& params) {
    // This prototype unlocks an existing session belonging to the SSH account.
    // It does not sign in a different account or create a new GUI session.
    return select_session(params);
}
Reply execute_system_desktop(json& request) {
    if(user_sid()!="S-1-5-18")throw Error("SYSTEM_REQUIRED","Secure desktop worker must run as LocalSystem");
    enable_privilege(SE_TCB_NAME);
    HWINSTA station=OpenWindowStationW(L"WinSta0",FALSE,WINSTA_ALL_ACCESS);check_win(station!=nullptr,"Open interactive SYSTEM station");
    check_win(SetProcessWindowStation(station),"Attach interactive SYSTEM station");
    auto op=request.at("op").get<std::string>();
    if(op=="display") {InputDesktop desktop(true);return execute_display_worker(request);}
    Handle unlockMutex;
    struct ReleaseMutexOnExit {HANDLE h=nullptr;~ReleaseMutexOnExit(){if(h)ReleaseMutex(h);}} unlockGuard;
    bool login=op=="login";
    if(op=="unlock" || login) {
        unlockMutex.reset(CreateMutexW(nullptr,FALSE,L"Local\\RemoteAgent.PasswordUnlock"));check_win(unlockMutex.valid(),"Create unlock mutex");
        auto wait=WaitForSingleObject(unlockMutex.get(),0);
        if(wait!=WAIT_OBJECT_0 && wait!=WAIT_ABANDONED)throw Error("DESKTOP_BUSY","Another Windows login/unlock is in progress; no password submitted");
        unlockGuard.h=unlockMutex.get();
    }
    struct WakeDisplay {bool active;explicit WakeDisplay(bool wake):active(wake){if(active){SetThreadExecutionState(ES_CONTINUOUS|ES_DISPLAY_REQUIRED|ES_SYSTEM_REQUIRED);DWORD_PTR ignored=0;SendMessageTimeoutW(HWND_BROADCAST,WM_SYSCOMMAND,SC_MONITORPOWER,static_cast<LPARAM>(-1),SMTO_ABORTIFHUNG,500,&ignored);Sleep(500);}}~WakeDisplay(){if(active)SetThreadExecutionState(ES_CONTINUOUS);}} awake(op=="unlock" || login);
    auto lockState=session_lock_state();bool locked=lockState["locked"].is_boolean() && lockState["locked"].get<bool>();
    InputDesktop desktop(true);auto name=object_name(desktop.value);
    json state={{"session_id",process_session()},{"input_desktop",name},{"window_station",object_name(GetProcessWindowStation())},{"wts",lockState},{"display",display_info()},{"experimental",true}};
    if(op=="desktop_probe") {
        Reply reply;
        try{reply=observe_input_desktop(request);}catch(const Error& e){reply=success(request,{{"observation_error",{{"code",e.code},{"message",e.what()}}}});}
        reply.header["result"].update(state);reply.header["result"]["desktops"]=desktop_inventory();return reply;
    }
    if(op!="unlock" && !login)throw Error("UNKNOWN_OPERATION","SYSTEM worker only supports desktop_probe, unlock and login");
    auto account=login?request.at("_login_account"):json::object();
    bool alreadySignedIn=false;
    if(login) {
        bool found=false;
        for(const auto& s:sessions())if(s.id==process_session()) {
            found=true;if(!s.user.empty() && s.sid!=request.value("_target_user_sid",std::string{}))throw Error("OTHER_USER_SESSION","The console belongs to another user");
            alreadySignedIn=!s.user.empty();
        }
        if(!found || WTSGetActiveConsoleSessionId()!=process_session())throw Error("LOGIN_NOT_READY","Console session changed before login");
        if(alreadySignedIn) {
            if(!lockState["locked"].is_boolean())throw Error("SESSION_STATE_UNKNOWN","Cannot determine whether the existing desktop is locked");
            if(locked)throw Error("USE_UNLOCK","This account is already signed in and locked; use unlock");
            return success(request,{{"session_id",process_session()},{"authenticated",true},{"desktop_ready",name=="Default"},{"already_signed_in",true},{"password_submitted",false}});
        }
    }
    if(!login && !lockState["locked"].is_boolean())throw Error("SESSION_STATE_UNKNOWN","Cannot determine the Windows session lock state; no password was submitted");
    auto& p=request["params"];
    struct Wipe {json& r;~Wipe(){erase_password(r);}} wipe{request};
    if(!login && name=="Default" && !locked)return success(request,{{"session_id",process_session()},{"unlocked",true},{"already_unlocked",true},{"password_submitted",false}});
    if(!login && !locked)throw Error("NOT_LOCKED","The session is not locked; unlock does not operate other secure-desktop prompts");
    if(!p.contains("password") || !p["password"].is_string())throw Error("INVALID_REQUEST","Password login/unlock requires params.password via stdin");
    auto password=wide(p["password"].get_ref<const std::string&>());
    struct WipeText {std::wstring& s;~WipeText(){if(!s.empty())SecureZeroMemory(s.data(),s.size()*sizeof(wchar_t));}} wipeText{password};
    if(password.empty() || password.size()>256 || password.find(L'\0')!=std::wstring::npos)throw Error("INVALID_REQUEST","Prototype password must contain 1..256 UTF-16 code units without NUL");
    // Only one credential submission. A space/mouse move can wake the curtain
    // without accidentally submitting an empty password on a transition frame.
    auto deadline=deadline_after(request["timeout_ms"].get<DWORD>()>5000?request["timeout_ms"].get<DWORD>()-3000:2000);
    if(name!="Winlogon") {
        // Let Windows dismiss its lock-screen curtain and perform the switch.
        // Forcing SwitchDesktop can expose an uninitialized LogonUI container.
        INPUT move{};move.type=INPUT_MOUSE;move.mi.dx=1;move.mi.dy=1;move.mi.dwFlags=MOUSEEVENTF_MOVE;SendInput(1,&move,sizeof(INPUT));send_key(VK_SPACE);
        for(int i=0;i<20 && input_name()!="Winlogon" && time_left(deadline)>6000;++i){Sleep(500);if(i==4 || i==9)send_key(VK_SHIFT);}
        HDESK active=OpenInputDesktop(0,FALSE,GENERIC_ALL);check_win(active!=nullptr,"Open awakened input desktop");
        auto awakeName=object_name(active);check_win(SetThreadDesktop(active),"Attach awakened input desktop");
        if(awakeName!="Winlogon")throw Error("PASSWORD_FIELD_UNAVAILABLE","Locked session has not activated the Winlogon desktop",{{"input_desktop",awakeName},{"password_submitted",false}});
    }
    PasswordField fields(account);
    bool priorErrorDismissed=false;
    if(fields.count==0 && fields.priorIncorrect && fields.priorErrorOk){
        ComPtr<IUIAutomationInvokePattern> invoke;check_hr(fields.priorErrorOk->GetCurrentPatternAs(UIA_InvokePatternId,IID_PPV_ARGS(&invoke)),"Logon error OK pattern");
        check_hr(invoke->Invoke(),"Dismiss prior incorrect-password notice");priorErrorDismissed=true;Sleep(500);fields=PasswordField(account);
    }
    if(fields.curtain){send_key(VK_SPACE);Sleep(500);fields=PasswordField(account);}
    if(fields.count==0){INPUT move{};move.type=INPUT_MOUSE;move.mi.dx=1;move.mi.dy=1;move.mi.dwFlags=MOUSEEVENTF_MOVE;SendInput(1,&move,sizeof(INPUT));send_key(VK_SHIFT);}
    for(int attempt=0;fields.count==0 && attempt<20 && time_left(deadline)>6000;++attempt){Sleep(500);fields=PasswordField(account);}
    if(fields.count!=1)throw Error("PASSWORD_FIELD_UNAVAILABLE","A unique Windows password field was not found; provider selection or secure attention may be required",{{"password_fields",fields.count},{"uia_nodes",fields.nodes},{"uia",fields.tree},{"password_candidates",fields.candidates},{"password_submitted",false}});
    if(login && !fields.accountSelected)throw Error("ACCOUNT_SELECTION_REQUIRED","The selected Windows account could not be confirmed; no password submitted",{{"account",account["account"]},{"uia",fields.tree},{"password_submitted",false}});
    if((!login && session_lock_state()["locked"]!=true) || input_name()!="Winlogon")throw Error("DESKTOP_CHANGED","Session lock state or input desktop changed before password input");
    bool sameAccount=false;
    for(const auto& s:sessions())if(s.id==process_session() && (login?s.user.empty() && WTSGetActiveConsoleSessionId()==s.id:s.active && s.sid==request.value("_target_user_sid",std::string{})))sameAccount=true;
    if(!sameAccount)throw Error("DESKTOP_CHANGED","Desktop account changed before password input; no password submitted");
    check_hr(fields.field->SetFocus(),"Focus Windows password field","UNSUPPORTED_LOGON_UI");
    ComPtr<IUIAutomationElement> focused;check_hr(fields.api->GetFocusedElement(&focused),"Check Windows password focus");
    BOOL same=FALSE;check_hr(fields.api->CompareElements(fields.field.Get(),focused.Get(),&same),"Compare password focus");
    if(!same)throw Error("FOCUS_FAILED","Windows password field did not gain focus");
    // Clear existing contents without reading them, then type and submit once.
    send_key(VK_HOME); // followed by Ctrl+A to cover provider-specific contents
    INPUT ctrl{};ctrl.type=INPUT_KEYBOARD;ctrl.ki.wVk=VK_CONTROL;
    if(SendInput(1,&ctrl,sizeof(INPUT))!=1)throw Error("INPUT_REJECTED","Control key rejected");
    try{send_key('A');}catch(...){ctrl.ki.dwFlags=KEYEVENTF_KEYUP;SendInput(1,&ctrl,sizeof(INPUT));throw;}
    ctrl.ki.dwFlags=KEYEVENTF_KEYUP;SendInput(1,&ctrl,sizeof(INPUT));send_key(VK_BACK);
    type_password(password);send_key(VK_RETURN);erase_password(request);
    bool authenticated=false;
    while(time_left(deadline)) {
        Sleep(200);
        if(login) {
            for(const auto& s:sessions())if(s.id==process_session() && s.active && s.sid==request.value("_target_user_sid",std::string{}))authenticated=true;
            bool ready=false;try{ready=authenticated && session_lock_state()["locked"]==false && input_name()=="Default";}catch(const Error&){}
            if(ready)return success(request,{{"session_id",process_session()},{"authenticated",true},{"desktop_ready",true},{"already_signed_in",false},{"password_submitted",true},{"account",account["account"]},{"experimental",true}});
            continue;
        }
        if(session_lock_state()["locked"]==false && input_name()=="Default")return success(request,{{"session_id",process_session()},{"unlocked",true},{"already_unlocked",false},{"password_submitted",true},{"prior_error_dismissed",priorErrorDismissed},{"experimental",true}});
    }
    if(login && authenticated)return success(request,{{"session_id",process_session()},{"authenticated",true},{"desktop_ready",false},{"already_signed_in",false},{"password_submitted",true},{"experimental",true}});
    if(login)throw Error("LOGIN_NOT_CONFIRMED","Password was submitted once, but Windows login was not confirmed; inspect before retrying",{{"password_submitted",true},{"outcome_unknown",true}});
    throw Error("UNLOCK_NOT_CONFIRMED","Password was submitted once, but the desktop did not become Default; inspect before retrying",{{"password_submitted",true},{"outcome_unknown",true}});
}
int system_launcher_main(const std::wstring& pipe,DWORD timeout,const std::wstring& task,DWORD session) {
    FreeConsole();Handle child;
    try {
        if(user_sid()!="S-1-5-18")return 2;
        ComPtr<ITaskService> service;check_hr(CoCreateInstance(CLSID_TaskScheduler,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&service)),"SYSTEM task cleanup");
        VARIANT empty{};check_hr(service->Connect(empty,empty,empty,empty),"SYSTEM task connection");
        ComPtr<ITaskFolder> folder;check_hr(service->GetFolder(Bstr(L"\\"),&folder),"SYSTEM task folder");check_hr(folder->DeleteTask(Bstr(task),0),"Remove SYSTEM task registration");
        enable_privilege(SE_TCB_NAME);enable_privilege(SE_INCREASE_QUOTA_NAME);enable_privilege(SE_ASSIGNPRIMARYTOKEN_NAME);enable_privilege(SE_DEBUG_NAME);
        // Use the target session's SYSTEM logon token, including its logon SID,
        // rather than transplanting the scheduler's Session 0 token alone.
        Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0));check_win(snapshot.valid(),"Find target Winlogon");
        PROCESSENTRY32W entry{};entry.dwSize=sizeof(entry);DWORD winlogon=0;
        if(Process32FirstW(snapshot.get(),&entry))do{DWORD id=MAXDWORD;if(_wcsicmp(entry.szExeFile,L"winlogon.exe")==0 && ProcessIdToSessionId(entry.th32ProcessID,&id) && id==session){winlogon=entry.th32ProcessID;break;}}while(Process32NextW(snapshot.get(),&entry));
        if(!winlogon)throw Error("SYSTEM_START_FAILED","Target session has no Winlogon process");
        Handle source(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|PROCESS_CREATE_PROCESS,FALSE,winlogon));check_win(source.valid(),"Open target Winlogon token source");
        HANDLE raw=nullptr;check_win(OpenProcessToken(source.get(),TOKEN_DUPLICATE|TOKEN_QUERY,&raw),"Open session SYSTEM token");Handle original(raw);
        check_win(DuplicateTokenEx(original.get(),MAXIMUM_ALLOWED,nullptr,SecurityImpersonation,TokenPrimary,&raw),"Duplicate SYSTEM token");Handle token(raw);
        check_win(SetTokenInformation(token.get(),TokenSessionId,&session,sizeof(session)),"Set SYSTEM target session");
        auto exe=exe_path();auto args=quote_arg(exe)+L" --worker --system --pipe "+quote_arg(pipe)+L" --timeout "+std::to_wstring(timeout)+L" --task "+quote_arg(task);
        SIZE_T attrSize=0;InitializeProcThreadAttributeList(nullptr,1,0,&attrSize);std::vector<uint8_t> attrs(attrSize);
        auto attr=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrs.data());check_win(InitializeProcThreadAttributeList(attr,1,0,&attrSize),"SYSTEM launch attributes");
        struct Attributes {LPPROC_THREAD_ATTRIBUTE_LIST p;~Attributes(){DeleteProcThreadAttributeList(p);}} attrCleanup{attr};
        HANDLE parent=source.get();check_win(UpdateProcThreadAttribute(attr,0,PROC_THREAD_ATTRIBUTE_PARENT_PROCESS,&parent,sizeof(parent),nullptr,nullptr),"SYSTEM session parent");
        Handle job(CreateJobObjectW(nullptr,nullptr));check_win(job.valid(),"SYSTEM worker lifetime job");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};limit.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        check_win(SetInformationJobObject(job.get(),JobObjectExtendedLimitInformation,&limit,sizeof(limit)),"SYSTEM worker lifetime limit");
        STARTUPINFOEXW si{};si.StartupInfo.cb=sizeof(si);si.StartupInfo.lpDesktop=const_cast<LPWSTR>(L"winsta0\\winlogon");si.lpAttributeList=attr;PROCESS_INFORMATION pi{};
        check_win(CreateProcessAsUserW(token.get(),exe.c_str(),args.data(),nullptr,nullptr,FALSE,CREATE_UNICODE_ENVIRONMENT|CREATE_SUSPENDED|EXTENDED_STARTUPINFO_PRESENT,nullptr,nullptr,&si.StartupInfo,&pi),"Start SYSTEM desktop worker");
        child.reset(pi.hProcess);Handle thread(pi.hThread);
        if(!AssignProcessToJobObject(job.get(),child.get())){DWORD e=GetLastError();TerminateProcess(child.get(),2);throw Error("SYSTEM_START_FAILED","Assign SYSTEM worker lifetime job: "+win_message(e));}
        if(ResumeThread(thread.get())==MAXDWORD){TerminateProcess(child.get(),2);throw Error("SYSTEM_START_FAILED","Resume SYSTEM worker failed");}
        if(WaitForSingleObject(child.get(),timeout)!=WAIT_OBJECT_0){TerminateProcess(child.get(),2);WaitForSingleObject(child.get(),2000);return 2;}
        return 0;
    }catch(const Error& e) {
        // Return startup failure through the same pipe instead of hiding it in a
        // 30-second timeout. No credentials have been sent at this point.
        Handle out(CreateFileW(pipe.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr));
        if(out.valid())try{send_frame(out.get(),{{"kind","startup_error"},{"code",e.code},{"message",e.what()}},{},deadline_after(2000));}catch(...){}
        if(child.valid())TerminateProcess(child.get(),2);return 2;
    }catch(...){if(child.valid())TerminateProcess(child.get(),2);return 2;}
}
}
