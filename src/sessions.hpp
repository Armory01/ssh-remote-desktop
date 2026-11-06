#pragma once
#include "common.hpp"
namespace ra {
struct Session { DWORD id; std::string user; std::string sid; std::string state; bool active; };
std::vector<Session> sessions();
Session select_session(const json& params);
Session select_console_session(const json& params,bool allow_empty);
json session_state(DWORD session);
json platform_info();
json local_account();
bool on_input_desktop();
void require_input_desktop();
json status();
void enable_dpi_awareness();
}
