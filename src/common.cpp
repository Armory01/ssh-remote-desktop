#include "common.hpp"
#include <sddl.h>
#include <objbase.h>
#include <algorithm>
#include <sstream>
#include <cstdio>
#include <io.h>
#include <fcntl.h>

namespace ra {
ComInit::ComInit() : hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) { check_hr(hr, "CoInitializeEx"); }
ComInit::~ComInit() { if(SUCCEEDED(hr)) CoUninitialize(); }
std::string utf8(const std::wstring& s) {
    if(s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    if(!n) throw Error("INVALID_TEXT", "Invalid UTF-16 text");
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}
std::wstring wide(const std::string& s) {
    if(s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if(!n) throw Error("INVALID_TEXT", "Input is not valid UTF-8");
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}
std::string win_message(DWORD code) {
    wchar_t* p = nullptr;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<LPWSTR>(&p), 0, nullptr);
    std::wstring s = n ? std::wstring(p, n) : L"Windows error";
    if(p) LocalFree(p);
    while(!s.empty() && (s.back() == L'\r' || s.back() == L'\n')) s.pop_back();
    return utf8(s) + " (" + std::to_string(code) + ")";
}
void check_win(BOOL ok, const char* what) {
    if(!ok) { DWORD e = GetLastError(); throw Error("WINDOWS_ERROR", std::string(what) + ": " + win_message(e), {{"win32", e}}); }
}
void check_hr(HRESULT hr, const char* what, const char* code) {
    if(FAILED(hr)) throw Error(code, std::string(what) + ": " + win_message(static_cast<DWORD>(hr)), {{"hresult", static_cast<uint32_t>(hr)}});
}
std::wstring exe_path() {
    std::vector<wchar_t> b(32768);
    DWORD n = GetModuleFileNameW(nullptr, b.data(), static_cast<DWORD>(b.size()));
    if(!n || n == b.size()) throw Error("WINDOWS_ERROR", "Cannot resolve helper executable path");
    return std::wstring(b.data(), n);
}
std::wstring quote_arg(const std::wstring& arg) {
    std::wstring out = L"\"";
    size_t slashes = 0;
    for(wchar_t c : arg) {
        if(c == L'\\') { ++slashes; continue; }
        if(c == L'\"') out.append(slashes * 2 + 1, L'\\');
        else out.append(slashes, L'\\');
        slashes = 0; out += c;
    }
    out.append(slashes * 2, L'\\'); out += L'\"';
    return out;
}
std::string random_id() {
    GUID id{}; check_hr(CoCreateGuid(&id), "CoCreateGuid");
    wchar_t b[40]{}; StringFromGUID2(id, b, 40);
    std::wstring s(b); return utf8(s.substr(1, s.size() - 2));
}
std::string user_sid(DWORD pid) {
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid));check_win(process.valid(),"Query process identity");
    HANDLE raw = nullptr; check_win(OpenProcessToken(process.get(), TOKEN_QUERY, &raw), "OpenProcessToken"); Handle token(raw);
    DWORD n = 0; GetTokenInformation(token.get(), TokenUser, nullptr, 0, &n);
    std::vector<uint8_t> data(n);
    check_win(GetTokenInformation(token.get(), TokenUser, data.data(), n, &n), "GetTokenInformation");
    LPWSTR text = nullptr; check_win(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid, &text), "ConvertSidToStringSid");
    std::wstring sid(text); LocalFree(text); return utf8(sid);
}
std::wstring account_name() {
    HANDLE raw = nullptr; check_win(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw), "OpenProcessToken"); Handle token(raw);
    DWORD n = 0; GetTokenInformation(token.get(), TokenUser, nullptr, 0, &n); std::vector<uint8_t> data(n);
    check_win(GetTokenInformation(token.get(), TokenUser, data.data(), n, &n), "GetTokenInformation");
    wchar_t name[512]{}, domain[512]{}; DWORD nn=512, dn=512; SID_NAME_USE use{};
    check_win(LookupAccountSidW(nullptr, reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid, name, &nn, domain, &dn, &use), "LookupAccountSid");
    return std::wstring(domain) + L"\\" + name;
}
DWORD process_session() { DWORD id=0; check_win(ProcessIdToSessionId(GetCurrentProcessId(), &id), "ProcessIdToSessionId"); return id; }
json Privileges::describe() const {
    const char* level=integrity_rid>=SECURITY_MANDATORY_SYSTEM_RID?"system":
        integrity_rid>=SECURITY_MANDATORY_HIGH_RID?"high":
        integrity_rid>=SECURITY_MANDATORY_MEDIUM_RID?"medium":
        integrity_rid>=SECURITY_MANDATORY_LOW_RID?"low":"untrusted";
    return {{"elevated",elevated},{"integrity_level",level},{"integrity_rid",integrity_rid}};
}
Privileges process_privileges(DWORD pid) {
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid));check_win(process.valid(),"Query process privileges");
    HANDLE raw=nullptr;check_win(OpenProcessToken(process.get(),TOKEN_QUERY,&raw),"Query process token");Handle token(raw);
    TOKEN_ELEVATION elevation{};DWORD size=0;
    check_win(GetTokenInformation(token.get(),TokenElevation,&elevation,sizeof(elevation),&size),"Query token elevation");
    GetTokenInformation(token.get(),TokenIntegrityLevel,nullptr,0,&size);std::vector<uint8_t> data(size);
    check_win(GetTokenInformation(token.get(),TokenIntegrityLevel,data.data(),size,&size),"Query token integrity");
    auto sid=reinterpret_cast<TOKEN_MANDATORY_LABEL*>(data.data())->Label.Sid;
    DWORD rid=*GetSidSubAuthority(sid,*GetSidSubAuthorityCount(sid)-1);
    return {elevation.TokenIsElevated!=0,rid};
}
json normalize_request(json request) {
    if(!request.is_object()) throw Error("INVALID_REQUEST", "Request must be an object");
    if(!request.contains("protocol")) request["protocol"] = 1;
    if(!request.contains("protocol") || !request["protocol"].is_number_integer() || request["protocol"] != 1)
        throw Error("PROTOCOL_MISMATCH", "Expected protocol 1");
    if(!request.contains("op") || !request["op"].is_string()) throw Error("INVALID_REQUEST", "op must be a string");
    if(!request.contains("id")) request["id"] = random_id();
    if(!request["id"].is_string() || request["id"].get<std::string>().size() > 128) throw Error("INVALID_REQUEST", "id must be a string of at most 128 bytes");
    if(!request.contains("params")) request["params"] = json::object();
    if(!request["params"].is_object()) throw Error("INVALID_REQUEST", "params must be an object");
    if(!request["params"].contains("elevated")) request["params"]["elevated"] = false;
    if(!request["params"]["elevated"].is_boolean()) throw Error("INVALID_REQUEST", "params.elevated must be a boolean");
    if(!request.contains("timeout_ms")) request["timeout_ms"] = request["op"]=="login"?120000:30000;
    if(!request["timeout_ms"].is_number_integer()) throw Error("INVALID_REQUEST", "timeout_ms must be an integer");
    int64_t t = request["timeout_ms"].get<int64_t>();
    if(t < 1000 || t > 120000) throw Error("INVALID_REQUEST", "timeout_ms must be between 1000 and 120000");
    return request;
}
Reply success(const json& request, json result) {
    return {{{"protocol",1},{"id",request.value("id",json(nullptr))},{"ok",true},{"result",std::move(result)},
        {"error",nullptr},{"image_bytes",0},{"mime_type",nullptr}}, {}};
}
Reply failure(const json& request, const Error& error) {
    return {{{"protocol",1},{"id",request.is_object()?request.value("id",json(nullptr)):json(nullptr)}, {"ok",false},{"result",nullptr},
        {"error",{{"code",error.code},{"message",error.what()},{"details",error.details}}},{"image_bytes",0},{"mime_type",nullptr}}, {}};
}
void write_stdout(const Reply& reply) {
    auto h = reply.header; h["image_bytes"] = reply.image.size();
    std::string line = h.dump(-1,' ',false,json::error_handler_t::replace) + "\n";
    auto write = [](const void* p, size_t count) {
        auto* b = static_cast<const uint8_t*>(p);
        while(count) { DWORD done=0; check_win(WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), b, static_cast<DWORD>(std::min<size_t>(count,65536)), &done,nullptr),"Write stdout"); if(!done) throw Error("IO_ERROR","stdout closed"); b+=done;count-=done; }
    };
    write(line.data(),line.size()); if(!reply.image.empty()) write(reply.image.data(),reply.image.size());
}
std::string read_stdin() {
    std::string out; char b[4096];
    for(;;) {DWORD n=0;if(!ReadFile(GetStdHandle(STD_INPUT_HANDLE),b,sizeof(b),&n,nullptr)){DWORD e=GetLastError();if(e==ERROR_BROKEN_PIPE)break;throw Error("IO_ERROR","stdin: "+win_message(e));}if(!n)break;out.append(b,n);if(out.size()>max_header)throw Error("INVALID_REQUEST","Request exceeds 1 MiB");}
    return out;
}
uint64_t deadline_after(DWORD ms) { return GetTickCount64()+ms; }
DWORD time_left(uint64_t deadline) { auto now=GetTickCount64(); return deadline>now?static_cast<DWORD>(std::min<uint64_t>(deadline-now,MAXDWORD-1)):0; }
std::pair<LONG,LONG> normalize_point(LONG x,LONG y,const RECT& r) {
    if(r.right<=r.left || r.bottom<=r.top || x<r.left || x>=r.right || y<r.top || y>=r.bottom) throw Error("INVALID_COORDINATES","Point is outside the virtual desktop");
    LONG nx=static_cast<LONG>(((static_cast<int64_t>(x)-r.left)*65536+32768)/(r.right-r.left));
    LONG ny=static_cast<LONG>(((static_cast<int64_t>(y)-r.top)*65536+32768)/(r.bottom-r.top));
    return {std::clamp<LONG>(nx,0,65535),std::clamp<LONG>(ny,0,65535)};
}
}
