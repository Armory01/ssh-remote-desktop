#include "bridge.hpp"
#include "sessions.hpp"
#include "desktop.hpp"
#include "system.hpp"
#include <taskschd.h>
#include <sddl.h>
#include <wtsapi32.h>
#include <algorithm>
#include <cstring>

namespace ra {
namespace {
void pipe_io(HANDLE h,void* buffer,size_t bytes,bool write,uint64_t deadline) {
    auto* p=static_cast<uint8_t*>(buffer);
    while(bytes) {
        DWORD left=time_left(deadline); if(!left) throw Error("TIMEOUT","Desktop worker communication timed out");
        Handle event(CreateEventW(nullptr,TRUE,FALSE,nullptr)); check_win(event.valid(),"CreateEvent");
        OVERLAPPED ov{};ov.hEvent=event.get(); DWORD done=0;
        DWORD want=static_cast<DWORD>(std::min<size_t>(bytes,65536));
        BOOL ok=write?WriteFile(h,p,want,&done,&ov):ReadFile(h,p,want,&done,&ov);
        if(!ok) {
            DWORD e=GetLastError();
            if(e!=ERROR_IO_PENDING) throw Error("WORKER_DISCONNECTED",win_message(e),{{"outcome_unknown",true}});
            DWORD waited=WaitForSingleObject(event.get(),left);
            if(waited!=WAIT_OBJECT_0) {
                CancelIoEx(h,&ov); GetOverlappedResult(h,&ov,&done,TRUE);
                throw Error("TIMEOUT","Desktop worker communication timed out",{{"outcome_unknown",true}});
            }
            if(!GetOverlappedResult(h,&ov,&done,FALSE)) throw Error("WORKER_DISCONNECTED",win_message(GetLastError()),{{"outcome_unknown",true}});
        }
        if(!done) throw Error("WORKER_DISCONNECTED","Desktop worker pipe closed",{{"outcome_unknown",true}});
        p+=done;bytes-=done;
    }
}
}
void send_frame(HANDLE pipe,const json& header,const std::vector<uint8_t>& image,uint64_t deadline) {
    json h=header;h["image_bytes"]=image.size();
    std::string text=h.dump(-1,' ',false,json::error_handler_t::replace);
    struct Wipe {json& h;std::string& s;~Wipe(){erase_password(h);if(!s.empty())SecureZeroMemory(s.data(),s.size());}} wipe{h,text};
    if(text.size()>max_header || image.size()>max_image) throw Error("RESULT_TOO_LARGE","Worker result exceeds protocol limits");
    uint32_t size=static_cast<uint32_t>(text.size());
    pipe_io(pipe,&size,sizeof(size),true,deadline);
    pipe_io(pipe,text.data(),text.size(),true,deadline);
    if(!image.empty()) pipe_io(pipe,const_cast<uint8_t*>(image.data()),image.size(),true,deadline);
}
Reply receive_frame(HANDLE pipe,uint64_t deadline) {
    uint32_t size=0;pipe_io(pipe,&size,sizeof(size),false,deadline);
    if(!size || size>max_header) throw Error("INVALID_WORKER_RESPONSE","Worker header length is invalid");
    std::string text(size,'\0');pipe_io(pipe,text.data(),size,false,deadline);
    struct Wipe {std::string& s;~Wipe(){if(!s.empty())SecureZeroMemory(s.data(),s.size());}} wipe{text};
    auto h=json::parse(text);
    if(!h.is_object() || !h.contains("image_bytes") || !h["image_bytes"].is_number_unsigned()) throw Error("INVALID_WORKER_RESPONSE","Invalid worker frame");
    auto n=h["image_bytes"].get<uint64_t>(); if(n>max_image) throw Error("RESULT_TOO_LARGE","Worker image exceeds 32 MiB");
    Reply out{std::move(h),std::vector<uint8_t>(static_cast<size_t>(n))};
    if(n) pipe_io(pipe,out.image.data(),out.image.size(),false,deadline);
    return out;
}
namespace {
// A SYSTEM task must not execute the user-writable deployment in the SSH home.
// Stage the identical single binary in a new protected directory for this call.
class SystemCopy {
    std::wstring directory_,file_;
public:
    explicit SystemCopy(bool needed) {
        if(!needed)return;
        wchar_t windows[32768]{};check_win(GetWindowsDirectoryW(windows,32768)!=0,"Resolve Windows directory");
        directory_=std::wstring(windows)+L"\\Temp\\RemoteAgent-"+wide(random_id());
        PSECURITY_DESCRIPTOR descriptor=nullptr;
        check_win(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)",SDDL_REVISION_1,&descriptor,nullptr),"Protected helper security");
        SECURITY_ATTRIBUTES sa{sizeof(sa),descriptor,FALSE};BOOL created=CreateDirectoryW(directory_.c_str(),&sa);DWORD error=GetLastError();LocalFree(descriptor);
        if(!created)throw Error("SYSTEM_STAGE_FAILED","Create protected helper directory: "+win_message(error));
        file_=directory_+L"\\ra.exe";
        if(!CopyFileW(exe_path().c_str(),file_.c_str(),TRUE)){error=GetLastError();RemoveDirectoryW(directory_.c_str());throw Error("SYSTEM_STAGE_FAILED","Copy protected helper: "+win_message(error));}
    }
    std::wstring executable() const{return file_.empty()?exe_path():file_;}
    ~SystemCopy(){if(!file_.empty())DeleteFileW(file_.c_str());if(!directory_.empty())RemoveDirectoryW(directory_.c_str());}
};
class Task {
    ComPtr<ITaskFolder> folder_;
    ComPtr<IRegisteredTask> task_;
    ComPtr<IRunningTask> running_;
    std::wstring name_;
    bool finished_=false;
public:
    Task(const std::wstring& pipe,const Session& session,DWORD timeout,bool elevated,bool system,const std::wstring& executable) {
        ComPtr<ITaskService> service;
        check_hr(CoCreateInstance(CLSID_TaskScheduler,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&service)),"Create Task Scheduler","DESKTOP_ACCESS_DENIED");
        VARIANT empty{};VariantInit(&empty);
        check_hr(service->Connect(empty,empty,empty,empty),"Connect Task Scheduler","DESKTOP_ACCESS_DENIED");
        check_hr(service->GetFolder(Bstr(L"\\"),&folder_),"Open Task Scheduler root","DESKTOP_ACCESS_DENIED");
        ComPtr<ITaskDefinition> definition;check_hr(service->NewTask(0,&definition),"NewTask");
        ComPtr<IPrincipal> principal;check_hr(definition->get_Principal(&principal),"Task principal");
        auto identity=system?std::wstring(L"S-1-5-18"):wide(session.user);
        auto logon=system?TASK_LOGON_SERVICE_ACCOUNT:TASK_LOGON_INTERACTIVE_TOKEN;
        check_hr(principal->put_UserId(Bstr(identity)),"Task user");
        check_hr(principal->put_LogonType(logon),"Task logon type");
        check_hr(principal->put_RunLevel(elevated?TASK_RUNLEVEL_HIGHEST:TASK_RUNLEVEL_LUA),"Task run level");
        ComPtr<ITaskSettings> settings;check_hr(definition->get_Settings(&settings),"Task settings");
        check_hr(settings->put_AllowDemandStart(VARIANT_TRUE),"AllowDemandStart");
        check_hr(settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE),"Task battery setting");
        check_hr(settings->put_StopIfGoingOnBatteries(VARIANT_FALSE),"Task battery setting");
        check_hr(settings->put_Hidden(VARIANT_TRUE),"Task hidden setting");
        check_hr(settings->put_Compatibility(TASK_COMPATIBILITY_V2),"Task compatibility");
        std::wstring duration=L"PT"+std::to_wstring((timeout+999)/1000+5)+L"S";
        check_hr(settings->put_ExecutionTimeLimit(Bstr(duration)),"Task execution limit");
        ComPtr<IActionCollection> actions;check_hr(definition->get_Actions(&actions),"Task actions");
        ComPtr<IAction> action;check_hr(actions->Create(TASK_ACTION_EXEC,&action),"Task action");
        ComPtr<IExecAction> exec;check_hr(action.As(&exec),"Task exec action");
        check_hr(exec->put_Path(Bstr(executable)),"Task executable");
        name_=L"RemoteAgent-"+wide(random_id());
        auto args=std::wstring(system?L"--system-launcher":L"--worker")+L" --pipe "+quote_arg(pipe)+L" --timeout "+std::to_wstring(timeout)+L" --task "+quote_arg(name_);
        if(system)args+=L" --session "+std::to_wstring(session.id);
        check_hr(exec->put_Arguments(Bstr(args)),"Task arguments");
        VARIANT user{};user.vt=VT_BSTR;user.bstrVal=SysAllocString(identity.c_str());
        VARIANT security{};security.vt=VT_BSTR;security.bstrVal=SysAllocString((L"D:P(A;;GA;;;SY)(A;;GA;;;"+wide(user_sid())+L")").c_str());
        HRESULT registered=folder_->RegisterTaskDefinition(Bstr(name_),definition.Get(),TASK_CREATE,user,empty,logon,security,&task_);
        VariantClear(&security);
        VariantClear(&user);
        check_hr(registered,"Register interactive task",elevated?"ELEVATION_DENIED":"DESKTOP_ACCESS_DENIED");
        HRESULT run=system?task_->Run(empty,&running_):task_->RunEx(empty,TASK_RUN_USE_SESSION_ID,static_cast<LONG>(session.id),nullptr,&running_);
        // Some policies permit own-user Run but reject the explicit-session flag.
        // The pipe handshake below still verifies the actual user and session.
        if(FAILED(run)) run=task_->Run(empty,&running_);
        if(FAILED(run)) { folder_->DeleteTask(Bstr(name_),0);name_.clear();check_hr(run,"Run interactive task","DESKTOP_ACCESS_DENIED"); }
    }
    void complete() { finished_=true; }
    ~Task() {
        if(!finished_ && running_) {running_->Stop();Sleep(100);}
        if(folder_ && !name_.empty()) { BSTR name=SysAllocString(name_.c_str());if(name){folder_->DeleteTask(name,0);SysFreeString(name);} }
    }
};
Reply run_in_session(const json& request,const Session& session,bool system=false) {
    DWORD timeout=request["timeout_ms"].get<DWORD>(); uint64_t deadline=deadline_after(timeout);
    auto sid=user_sid();std::wstring name=L"\\\\.\\pipe\\RemoteAgent."+wide(sid)+L"."+wide(random_id());
    std::wstring sddl=L"D:P(A;;GA;;;SY)(A;;GA;;;"+wide(sid)+L")";
    PSECURITY_DESCRIPTOR descriptor=nullptr;
    check_win(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),SDDL_REVISION_1,&descriptor,nullptr),"Pipe security descriptor");
    SECURITY_ATTRIBUTES sa{sizeof(sa),descriptor,FALSE};
    Handle pipe(CreateNamedPipeW(name.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,65536,65536,0,&sa));
    DWORD pipeError=GetLastError();LocalFree(descriptor);
    if(!pipe.valid()) throw Error("IO_ERROR","Create desktop pipe: "+win_message(pipeError));
    Handle event(CreateEventW(nullptr,TRUE,FALSE,nullptr));check_win(event.valid(),"Create pipe event");
    OVERLAPPED connection{};connection.hEvent=event.get();
    BOOL connected=ConnectNamedPipe(pipe.get(),&connection);DWORD connectError=connected?ERROR_SUCCESS:GetLastError();
    if(!connected && connectError!=ERROR_IO_PENDING && connectError!=ERROR_PIPE_CONNECTED) check_win(FALSE,"ConnectNamedPipe");
    struct CancelConnect {HANDLE pipe;OVERLAPPED* ov;bool pending;~CancelConnect(){if(pending){CancelIoEx(pipe,ov);DWORD ignored=0;GetOverlappedResult(pipe,ov,&ignored,TRUE);}}} cleanup{pipe.get(),&connection,!connected && connectError==ERROR_IO_PENDING};
    bool elevated=request["params"].value("elevated",false);
    SystemCopy copy(system);
    Task task(name,session,timeout,elevated||system,system,copy.executable());
    if(!connected && connectError==ERROR_IO_PENDING) {
        if(WaitForSingleObject(event.get(),time_left(deadline))!=WAIT_OBJECT_0) {
            CancelIoEx(pipe.get(),&connection);DWORD ignored=0;GetOverlappedResult(pipe.get(),&connection,&ignored,TRUE);
            throw Error("WORKER_START_TIMEOUT","The desktop worker did not connect; check desktop logon and task policy");
        }
        DWORD ignored=0;check_win(GetOverlappedResult(pipe.get(),&connection,&ignored,FALSE),"Accept desktop worker");
        cleanup.pending=false;
    }
    auto hello=receive_frame(pipe.get(),deadline).header;
    if(hello.value("kind",std::string{})=="startup_error")throw Error(hello.value("code","SYSTEM_START_FAILED"),hello.value("message","SYSTEM worker failed to start"));
    auto expectedSid=system?std::string("S-1-5-18"):sid;
    if(hello.value("kind",std::string{})!="hello" || hello.value("session_id",MAXDWORD)!=session.id || hello.value("user_sid",std::string{})!=expectedSid)
        throw Error("INVALID_WORKER_RESPONSE","Desktop worker identity or session mismatch");
    DWORD pid=hello.at("pid").get<DWORD>(); Handle worker(OpenProcess(SYNCHRONIZE,FALSE,pid));
    ULONG actualPid=0;DWORD actualSession=MAXDWORD;
    check_win(GetNamedPipeClientProcessId(pipe.get(),&actualPid),"Query actual pipe client");
    if(actualPid!=pid || !ProcessIdToSessionId(pid,&actualSession) || actualSession!=session.id || user_sid(pid)!=expectedSid)
        throw Error("INVALID_WORKER_RESPONSE","Kernel-reported worker identity does not match the expected worker");
    if(elevated && !process_privileges(pid).high())throw Error("ELEVATION_FAILED","Scheduler did not start a high-integrity worker; no desktop actions were sent");
    json forwarded=request;
    if(system)forwarded["_target_user_sid"]=request["op"]=="login"?user_sid():session.sid;
    if(system && request["op"]=="login")forwarded["_login_account"]=local_account();
    struct WipeRequest {json& r;~WipeRequest(){erase_password(r);}} wipeForwarded{forwarded};
    send_frame(pipe.get(),forwarded,{},deadline);
    auto result=receive_frame(pipe.get(),deadline);
    if(result.header.value("id",json(nullptr))!=request["id"]) throw Error("INVALID_WORKER_RESPONSE","Worker request ID mismatch");
    if(worker.valid() && WaitForSingleObject(worker.get(),std::min<DWORD>(time_left(deadline),2000))!=WAIT_OBJECT_0)
        throw Error("WORKER_EXIT_TIMEOUT","Worker returned a result but did not exit");
    // The SYSTEM launcher waits for its desktop child. Ensure it has released
    // the protected executable before deleting the per-call staging directory.
    if(system)Sleep(200);
    task.complete();return result;
}
}
Reply dispatch(const json& request) {
    auto op=request.at("op").get<std::string>();
    if(op=="status") return success(request,status());
    if(op=="desktop_probe" || op=="unlock" || op=="login") {
        if(!process_privileges().high())throw Error("ELEVATION_REQUIRED","An elevated SSH/helper process is required for the temporary SYSTEM worker");
        if(op=="login") {
            auto os=platform_info();if(os["major"]!=10 || os["build"].get<DWORD>()<22000 || os["workstation"]!=true)throw Error("UNSUPPORTED_PLATFORM","Password login currently supports Windows 11 only");
            auto account=local_account();const auto& p=request["params"];
            if(p.contains("user")) {
                if(!p["user"].is_string())throw Error("INVALID_REQUEST","params.user must be a local account name");
                auto requested=wide(p["user"].get<std::string>()),name=wide(account["name"].get<std::string>()),full=wide(account["account"].get<std::string>());
                if(_wcsicmp(requested.c_str(),name.c_str())!=0 && _wcsicmp(requested.c_str(),full.c_str())!=0 && _wcsicmp(requested.c_str(),(L".\\"+name).c_str())!=0)throw Error("UNSUPPORTED_ACCOUNT","login is limited to the SSH account; no credentials were submitted");
            }
        }
        const auto& params=request["params"];
        bool consoleProbe=op=="desktop_probe" && (!params.contains("session_id") || (params["session_id"].is_number_integer() && params["session_id"].get<int64_t>()==WTSGetActiveConsoleSessionId()));
        auto session=op=="login" || consoleProbe?select_console_session(params,true):select_unlock_session(params);
        auto reply=run_in_session(request,session,true);
        if(reply.header.value("ok",false) && op=="unlock") {
            // Verify the account as well as the input desktop after Winlogon.
            auto current=select_unlock_session(request["params"]);
            if(current.id!=session.id || current.sid!=session.sid)throw Error("UNLOCK_NOT_CONFIRMED","Desktop account changed while unlocking",{{"outcome_unknown",true}});
        }
        if(reply.header.value("ok",false) && op=="login") {
            auto current=select_console_session(request["params"],false);
            if(current.id!=session.id || current.sid!=user_sid())throw Error("LOGIN_NOT_CONFIRMED","The console account or session changed while signing in",{{"outcome_unknown",true}});
        }
        return reply;
    }
    if(op!="windows" && op!="observe" && op!="act" && op!="launch") throw Error("UNKNOWN_OPERATION","Unknown operation: "+op);
    bool elevated=request["params"].value("elevated",false);auto caller=process_privileges();
    if(elevated && !caller.high())throw Error("ELEVATION_REQUIRED","An elevated SSH/helper process is required to request a high-integrity worker",{{"caller",caller.describe()}});
    auto session=select_session(request["params"]);
    if(process_session()==session.id && on_input_desktop() && caller.high()==elevated && !request["params"].value("force_bridge",false)) return execute_desktop(request);
    return run_in_session(request,session);
}
int worker_main(const std::wstring& name,DWORD timeout,const std::wstring& task,bool system) {
    // The GUI subsystem avoids a task console; detach any inherited console.
    FreeConsole();
    auto deadline=deadline_after(timeout);
    Handle pipe;
    while(time_left(deadline)) {
        pipe.reset(CreateFileW(name.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr));
        if(pipe.valid()) break;
        DWORD e=GetLastError(); if(e!=ERROR_PIPE_BUSY && e!=ERROR_FILE_NOT_FOUND) return 2;
        Sleep(20);
    }
    if(!pipe.valid()) return 2;
    json request=json::object();
    try {
        send_frame(pipe.get(),{{"kind","hello"},{"session_id",process_session()},{"user_sid",user_sid()},{"pid",GetCurrentProcessId()}},{},deadline);
        request=normalize_request(receive_frame(pipe.get(),deadline).header);
        Reply result;
        try {
            // Remove registration before any potentially blocking UI provider call.
            // The running instance remains tracked by Scheduler; the parent can
            // still stop it via IRunningTask. This also covers an SSH parent crash.
            if(system)result=execute_system_desktop(request);
            else {
            ComPtr<ITaskService> service;check_hr(CoCreateInstance(CLSID_TaskScheduler,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&service)),"Worker task cleanup");
            VARIANT empty{};check_hr(service->Connect(empty,empty,empty,empty),"Worker task cleanup connection");
            ComPtr<ITaskFolder> folder;check_hr(service->GetFolder(Bstr(L"\\"),&folder),"Worker task cleanup folder");
            check_hr(folder->DeleteTask(Bstr(task),0),"Worker task cleanup registration");
            result=execute_desktop(request);
            }
        }
        catch(const Error& e) { result=failure(request,e); }
        catch(const std::exception& e) { result=failure(request,Error("INTERNAL_ERROR",e.what())); }
        erase_password(request);
        if(system){result.header["worker"]=process_privileges().describe();result.header["worker"]["session_id"]=process_session();result.header["worker"]["pid"]=GetCurrentProcessId();}
        send_frame(pipe.get(),result.header,result.image,deadline);
        return result.header.value("ok",false)?0:1;
    } catch(...) { return 2; }
}
}
