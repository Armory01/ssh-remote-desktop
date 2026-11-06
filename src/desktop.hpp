#pragma once
#include "common.hpp"
namespace ra {
Reply execute_desktop(const json& request);
// The SYSTEM prototype has already attached its thread to the input desktop.
Reply observe_input_desktop(const json& request);
}
