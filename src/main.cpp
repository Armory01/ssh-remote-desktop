#include "bridge.hpp"
#include "sessions.hpp"
#include "system.hpp"
#include <iostream>
#include <shellapi.h>

static int run(int argc,wchar_t** argv) {
    ra::json request=ra::json::object();
    struct WipeRequest {ra::json& r;~WipeRequest(){ra::erase_password(r);}} wipe{request};
    try {
        ra::enable_dpi_awareness();ra::ComInit com;
        if(argc>1 && (std::wstring(argv[1])==L"--worker" || std::wstring(argv[1])==L"--system-launcher")) {
            bool launcher=std::wstring(argv[1])==L"--system-launcher",system=false;
            std::wstring pipe,task;DWORD timeout=30000,session=MAXDWORD;
            for(int i=2;i<argc;++i) {
                std::wstring arg=argv[i];
                if(arg==L"--pipe" && i+1<argc) pipe=argv[++i];
                else if(arg==L"--task" && i+1<argc) task=argv[++i];
                else if(arg==L"--timeout" && i+1<argc) timeout=std::stoul(argv[++i]);
                else if(arg==L"--session" && i+1<argc) session=std::stoul(argv[++i]);
                else if(arg==L"--system")system=true;
            }
            if(pipe.rfind(L"\\\\.\\pipe\\RemoteAgent.",0)!=0) return 2;
            if(task.size()!=48 || task.rfind(L"RemoteAgent-",0)!=0)return 2;
            if(launcher){if(session==MAXDWORD || session==0)return 2;return ra::system_launcher_main(pipe,timeout,task,session);}
            return ra::worker_main(pipe,timeout,task,system);
        }
        if(argc<2 || std::wstring(argv[1])==L"--help") {
            std::string help=std::string("SSH Remote Desktop ")+ra::version+"\nra status | display | windows | observe | desktop_probe | launch --exe PATH [--arg VALUE] | act | call\n"
                "Desktop commands accept --elevated (requires an already elevated caller).\n"
                "Windows login/unlock use call with credentials on stdin, never password arguments.\n"
                "call/act: one UTF-8 JSON request on stdin, followed by EOF. Output: JSON line + image_bytes raw bytes.\n";
            DWORD written=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),help.data(),static_cast<DWORD>(help.size()),&written,nullptr);
            return 0;
        }
        std::wstring command=argv[1];
        if(command==L"call" || command==L"act") {
            auto text=ra::read_stdin();
            struct WipeText {std::string& s;~WipeText(){if(!s.empty())SecureZeroMemory(s.data(),s.size());}} wipeInput{text};
            try{request=ra::json::parse(text);}catch(const ra::json::parse_error&){throw ra::Error("INVALID_REQUEST","stdin must contain a valid JSON request");}
        }
        else {
            request={{"protocol",1},{"op",ra::utf8(command)},{"params",ra::json::object()}};
            for(int i=2;i<argc;++i) {
                std::wstring arg=argv[i];
                auto next=[&]()->std::wstring {if(i+1>=argc)throw ra::Error("INVALID_REQUEST","Missing argument value");return argv[++i];};
                if(arg==L"--exe") request["params"]["exe"]=ra::utf8(next());
                else if(arg==L"--arg") { if(!request["params"].contains("args"))request["params"]["args"]=ra::json::array();request["params"]["args"].push_back(ra::utf8(next())); }
                else if(arg==L"--window") request["params"]["window"]=std::stoull(next(),nullptr,0);
                else if(arg==L"--session") request["params"]["session_id"]=std::stoul(next());
                else if(arg==L"--timeout") request["timeout_ms"]=std::stoul(next());
                else if(arg==L"--no-uia") request["params"]["uia"]=false;
                else if(arg==L"--no-image") request["params"]["screenshot"]=false;
                else if(arg==L"--force-bridge") request["params"]["force_bridge"]=true;
                else if(arg==L"--elevated") request["params"]["elevated"]=true;
                else throw ra::Error("INVALID_REQUEST","Unknown argument: "+ra::utf8(arg));
            }
        }
        request=ra::normalize_request(std::move(request));
        ra::Reply reply;
        try { reply=ra::dispatch(request); }
        catch(const ra::Error& e) { reply=ra::failure(request,e); }
        catch(const std::exception& e) { reply=ra::failure(request,ra::Error("INVALID_REQUEST",e.what())); }
        ra::erase_password(request);ra::write_stdout(reply);return reply.header.value("ok",false)?0:1;
    } catch(const ra::Error& e) {
        try{ra::write_stdout(ra::failure(request,e));}catch(...){}return 1;
    } catch(const std::exception& e) {
        try{ra::write_stdout(ra::failure(request,ra::Error("INVALID_REQUEST",e.what())));}catch(...){}return 1;
    }
}

int WINAPI wWinMain(HINSTANCE,HINSTANCE,LPWSTR,int) {
    int argc=0;wchar_t** argv=CommandLineToArgvW(GetCommandLineW(),&argc);if(!argv)return 2;
    // A GUI subsystem prevents Scheduler from creating a transient console over
    // the desktop. SSH's redirected standard handles still carry the CLI wire.
    // For direct terminal use, attach only if no output handle was inherited.
    bool worker=argc>1 && (std::wstring(argv[1])==L"--worker" || std::wstring(argv[1])==L"--system-launcher");
    HANDLE output=GetStdHandle(STD_OUTPUT_HANDLE);
    if(!worker && (!output || output==INVALID_HANDLE_VALUE))AttachConsole(ATTACH_PARENT_PROCESS);
    int result=run(argc,argv);LocalFree(argv);return result;
}
