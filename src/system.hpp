#pragma once
#include "common.hpp"
#include "sessions.hpp"
namespace ra {
Session select_unlock_session(const json& params);
Reply execute_system_desktop(json& request);
void erase_password(json& request);
}
