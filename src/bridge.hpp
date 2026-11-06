#pragma once
#include "common.hpp"
namespace ra {
Reply dispatch(const json& request);
int worker_main(const std::wstring& pipe,DWORD timeout,const std::wstring& task,bool system=false);
int system_launcher_main(const std::wstring& pipe,DWORD timeout,const std::wstring& task,DWORD session);
void send_frame(HANDLE pipe,const json& header,const std::vector<uint8_t>& image,uint64_t deadline);
Reply receive_frame(HANDLE pipe,uint64_t deadline);
}
