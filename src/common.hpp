#pragma once
#include <windows.h>
#include <wrl/client.h>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <stdexcept>
#include <cstdint>

namespace ra {
using json = nlohmann::json;
using Microsoft::WRL::ComPtr;
inline constexpr const char* version = "0.4.2";
inline constexpr size_t max_header = 1024 * 1024;
inline constexpr size_t max_image = 32 * 1024 * 1024;

struct Error : std::runtime_error {
    std::string code;
    json details;
    Error(std::string c, std::string message, json d = json::object())
        : std::runtime_error(std::move(message)), code(std::move(c)), details(std::move(d)) {}
};
class Handle {
    HANDLE value_ = nullptr;
public:
    explicit Handle(HANDLE h = nullptr) : value_(h) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value_(other.release()) {}
    Handle& operator=(Handle&& other) noexcept { if(this != &other) reset(other.release()); return *this; }
    bool valid() const { return value_ && value_ != INVALID_HANDLE_VALUE; }
    HANDLE get() const { return value_; }
    HANDLE release() { HANDLE h = value_; value_ = nullptr; return h; }
    void reset(HANDLE h = nullptr) { if(valid()) CloseHandle(value_); value_ = h; }
};
class Bstr {
    BSTR value_;
public:
    explicit Bstr(const std::wstring& s) : value_(SysAllocStringLen(s.data(), static_cast<UINT>(s.size()))) {
        if(!value_) throw std::bad_alloc();
    }
    ~Bstr() { SysFreeString(value_); }
    operator BSTR() const { return value_; }
    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;
};
struct ComInit {
    HRESULT hr;
    ComInit();
    ~ComInit();
};
struct Reply { json header; std::vector<uint8_t> image; };
struct Privileges {
    bool elevated;
    DWORD integrity_rid;
    bool high() const { return elevated && integrity_rid >= SECURITY_MANDATORY_HIGH_RID; }
    json describe() const;
};
Privileges process_privileges(DWORD pid = GetCurrentProcessId());
std::string utf8(const std::wstring& s);
std::wstring wide(const std::string& s);
std::string win_message(DWORD code);
void check_win(BOOL ok, const char* what);
void check_hr(HRESULT hr, const char* what, const char* code = "WINDOWS_ERROR");
std::wstring exe_path();
std::wstring quote_arg(const std::wstring& arg);
std::string random_id();
std::string user_sid(DWORD pid = GetCurrentProcessId());
std::wstring account_name();
DWORD process_session();
json normalize_request(json request);
Reply success(const json& request, json result = json::object());
Reply failure(const json& request, const Error& error);
void write_stdout(const Reply& reply);
std::string read_stdin();
uint64_t deadline_after(DWORD milliseconds);
DWORD time_left(uint64_t deadline);
std::pair<LONG,LONG> normalize_point(LONG x, LONG y, const RECT& desktop);
}
