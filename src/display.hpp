#pragma once
#include "common.hpp"
namespace ra {
// Machine-wide installation is performed by the elevated SSH front end;
// topology and capture are evaluated in the console-session SYSTEM worker.
json virtual_display_devices();
json install_virtual_display(const json& params);
json change_virtual_device(const std::string& action);
Reply execute_display_worker(const json& request);
}
