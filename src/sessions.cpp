#include "sessions.hpp"
#include <wtsapi32.h>
#include <sddl.h>
#include <lm.h>

namespace ra {
json platform_info() {
    OSVERSIONINFOEXW os{};os.dwOSVersionInfoSize=sizeof(os);
    using GetVersion=LONG(WINAPI*)(OSVERSIONINFOW*);
    auto getVersion=reinterpret_cast<GetVersion>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"RtlGetVersion"));
    if(!getVersion || getVersion(reinterpret_cast<OSVERSIONINFOW*>(&os))!=0)throw Error("WINDOWS_ERROR","Cannot determine Windows version");
    SYSTEM_INFO info{};GetNativeSystemInfo(&info);
    const char* arch=info.wProcessorArchitecture==PROCESSOR_ARCHITECTURE_AMD64?"x64":info.wProcessorArchitecture==PROCESSOR_ARCHITECTURE_INTEL?"x86":info.wProcessorArchitecture==PROCESSOR_ARCHITECTURE_ARM64?"arm64":"unknown";
    return {{"major",os.dwMajorVersion},{"minor",os.dwMinorVersion},{"build",os.dwBuildNumber},{"workstation",os.wProductType==VER_NT_WORKSTATION},{"architecture",arch}};
}
json local_account() {
    auto account=account_name();auto slash=account.find(L'\\');
    wchar_t computer[256]{};DWORD n=256;check_win(GetComputerNameW(computer,&n),"Resolve local computer");
    if(slash==std::wstring::npos || _wcsicmp(account.substr(0,slash).c_str(),computer)!=0)throw Error("UNSUPPORTED_ACCOUNT","Password login currently supports the SSH account as a local Windows account only");
    auto name=account.substr(slash+1);LPBYTE raw=nullptr;
    NET_API_STATUS result=NetUserGetInfo(nullptr,name.c_str(),4,&raw);
    if(result!=NERR_Success)throw Error("UNSUPPORTED_ACCOUNT","Cannot resolve the local Windows account",{{"win32",result}});
    auto info=reinterpret_cast<USER_INFO_4*>(raw);
    json out={{"account",utf8(account)},{"name",utf8(name)},{"display_name",info->usri4_full_name?utf8(info->usri4_full_name):std::string{}},{"sid",user_sid()}};
    NetApiBufferFree(raw);
    // LogonUI may display a full name instead of the account name. Only accept
    // a label that uniquely identifies this account among local accounts.
    json labels=json::array();
    for(const auto& label:{out["name"].get<std::string>(),out["display_name"].get<std::string>()}) {
        if(label.empty())continue;DWORD resume=0;bool unique=true;
        do {
            LPBYTE users=nullptr;DWORD read=0,total=0;NET_API_STATUS status=NetUserEnum(nullptr,2,FILTER_NORMAL_ACCOUNT,&users,MAX_PREFERRED_LENGTH,&read,&total,&resume);
            if(status!=NERR_Success && status!=ERROR_MORE_DATA){if(users)NetApiBufferFree(users);unique=false;break;}
            auto list=reinterpret_cast<USER_INFO_2*>(users);
            for(DWORD i=0;i<read;++i)if(_wcsicmp(list[i].usri2_name,name.c_str())!=0 && (_wcsicmp(list[i].usri2_name,wide(label).c_str())==0 || (list[i].usri2_full_name && _wcsicmp(list[i].usri2_full_name,wide(label).c_str())==0)))unique=false;
            NetApiBufferFree(users);if(status!=ERROR_MORE_DATA)break;
        }while(unique);
        if(unique && std::find(labels.begin(),labels.end(),json(label))==labels.end())labels.push_back(label);
    }
    out["ui_labels"]=labels;return out;
}
json session_state(DWORD id) {
    LPWSTR raw=nullptr;DWORD size=0;
    if(!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE,id,WTSSessionInfoEx,&raw,&size))return {{"locked",nullptr}};
    auto info=reinterpret_cast<WTSINFOEXW*>(raw);json result={{"locked",nullptr}};
    if(size>=sizeof(WTSINFOEXW) && info->Level==1) {
        auto flags=info->Data.WTSInfoExLevel1.SessionFlags;
        result["session_flags"]=flags;result["station"]=utf8(info->Data.WTSInfoExLevel1.WinStationName);
        OSVERSIONINFOW os{};os.dwOSVersionInfoSize=sizeof(os);
        using GetVersion=LONG(WINAPI*)(OSVERSIONINFOW*);
        auto getVersion=reinterpret_cast<GetVersion>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"RtlGetVersion"));
        if(getVersion && getVersion(&os)==0 && (flags==0 || flags==1)) {
            // Microsoft's documented Win7/2008 R2 reversal of LOCK and UNLOCK.
            bool reversed=os.dwMajorVersion==6 && os.dwMinorVersion==1;
            result["locked"]=reversed?flags==1:flags==0;
        }
    }
    WTSFreeMemory(raw);return result;
}
static std::wstring query(DWORD id,WTS_INFO_CLASS kind) {
    LPWSTR text=nullptr; DWORD n=0;
    if(!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE,id,kind,&text,&n)) return {};
    std::wstring result(text); WTSFreeMemory(text); return result;
}
static std::string sid_of(const std::wstring& account) {
    DWORD n=0,d=0; SID_NAME_USE use{}; LookupAccountNameW(nullptr,account.c_str(),nullptr,&n,nullptr,&d,&use);
    if(!n) return {};
    std::vector<uint8_t> sid(n); std::vector<wchar_t> domain(d);
    if(!LookupAccountNameW(nullptr,account.c_str(),sid.data(),&n,domain.data(),&d,&use)) return {};
    LPWSTR text=nullptr; if(!ConvertSidToStringSidW(sid.data(),&text)) return {};
    std::string s=utf8(text); LocalFree(text); return s;
}
std::vector<Session> sessions() {
    PWTS_SESSION_INFOW list=nullptr; DWORD count=0;
    check_win(WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE,0,1,&list,&count),"WTSEnumerateSessions");
    std::vector<Session> out;
    for(DWORD i=0;i<count;++i) {
        auto user=query(list[i].SessionId,WTSUserName);
        auto domain=query(list[i].SessionId,WTSDomainName);
        auto account=user.empty()?std::wstring{}:domain.empty()?user:domain+L"\\"+user;
        auto state=list[i].State;
        out.push_back({list[i].SessionId,utf8(account),account.empty()?std::string{}:sid_of(account),state==WTSActive?"active":state==WTSDisconnected?"disconnected":state==WTSConnected?"connected":state==WTSListen?"listen":"unavailable",state==WTSActive});
    }
    WTSFreeMemory(list); return out;
}
Session select_console_session(const json& params,bool allow_empty) {
    DWORD console=WTSGetActiveConsoleSessionId();
    if(console==MAXDWORD || console==0)throw Error("LOGIN_NOT_READY","No console session is currently attached; wait for Windows startup or logout to finish");
    if(params.contains("session_id") && (!params["session_id"].is_number_integer() || params["session_id"].get<int64_t>()!=console))throw Error("INVALID_REQUEST","Console login/probe must target the current console_session_id");
    for(const auto& s:sessions())if(s.id==console) {
        if(!s.user.empty() && s.sid!=user_sid())throw Error("OTHER_USER_SESSION","The console belongs to a different user; no account switching or logout was performed");
        if(s.user.empty() && !allow_empty)throw Error("NO_INTERACTIVE_SESSION","No user is signed in to the console");
        if(s.user.empty())for(const auto& other:sessions())if(!other.user.empty() && other.sid==user_sid())throw Error("SESSION_DISCONNECTED","An existing user session must be handled before signing in at the console");
        return s;
    }
    throw Error("LOGIN_NOT_READY","The console session is not ready yet");
}
Session select_session(const json& params) {
    auto sid=user_sid(); auto all=sessions(); std::vector<Session> candidates; bool disconnected=false;
    for(const auto& s:all) {
        if(s.sid!=sid) continue;
        if(s.state=="disconnected") disconnected=true;
        if(!s.active) continue;
        if(params.contains("session_id")) {
            if(!params["session_id"].is_number_unsigned() && !params["session_id"].is_number_integer()) throw Error("INVALID_REQUEST","session_id must be an integer");
            if(params["session_id"].get<int64_t>() != s.id) continue;
        }
        candidates.push_back(s);
    }
    if(candidates.empty()) throw Error(disconnected?"SESSION_DISCONNECTED":"NO_INTERACTIVE_SESSION",disconnected?"The user's desktop session is disconnected":"Log on to a Windows desktop using the SSH account first");
    if(candidates.size()>1) { json ids=json::array();for(const auto& s:candidates)ids.push_back(s.id);throw Error("AMBIGUOUS_SESSION","Specify params.session_id",{{"sessions",ids}}); }
    return candidates.front();
}
static std::wstring object_name(HANDLE h) {
    wchar_t b[512]{}; DWORD n=0;
    if(!GetUserObjectInformationW(h,UOI_NAME,b,sizeof(b),&n)) return {};
    return b;
}
bool on_input_desktop() {
    auto state=session_state(process_session());
    if(state["locked"].is_boolean() && state["locked"].get<bool>())return false;
    if(object_name(GetProcessWindowStation())!=L"WinSta0") return false;
    auto current=object_name(GetThreadDesktop(GetCurrentThreadId()));
    HDESK input=OpenInputDesktop(0,FALSE,DESKTOP_READOBJECTS|DESKTOP_SWITCHDESKTOP);
    if(!input) return false;
    auto name=object_name(input); CloseDesktop(input);
    return current==L"Default" && _wcsicmp(name.c_str(),L"Default")==0;
}
void require_input_desktop() {
    if(!on_input_desktop()) throw Error("DESKTOP_LOCKED","The interactive input desktop is unavailable; unlock the desktop and retry");
}
json status() {
    json all=json::array(); auto sid=user_sid();
    for(const auto& s:sessions()) all.push_back({{"id",s.id},{"user",s.user},{"same_user",s.sid==sid},{"state",s.state},{"locked",session_state(s.id)["locked"]}});
    wchar_t hostname[256]{}; DWORD n=256; GetComputerNameW(hostname,&n);
    return {{"version",version},{"protocol",1},{"hostname",utf8(hostname)},{"account",utf8(account_name())},{"user_sid",sid},{"os",platform_info()},
        {"process_session_id",process_session()},{"console_session_id",WTSGetActiveConsoleSessionId()},{"uptime_ms",GetTickCount64()},{"privileges",process_privileges().describe()},{"input_desktop",on_input_desktop()},{"sessions",all},
        {"capabilities",{"windows","observe","act","launch","uia","png","jpeg","batch","elevated_worker","experimental_desktop_probe","experimental_password_unlock","experimental_local_password_login","experimental_virtual_display"}}};
}
void enable_dpi_awareness() {
    HMODULE user=GetModuleHandleW(L"user32.dll");
    using SetContext=BOOL(WINAPI*)(HANDLE);
    auto fn=reinterpret_cast<SetContext>(GetProcAddress(user,"SetProcessDpiAwarenessContext"));
    if(fn && fn(reinterpret_cast<HANDLE>(-4))) return;
    SetProcessDPIAware();
}
}
