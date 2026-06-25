#pragma once

#include <string>

namespace toycc {

[[noreturn]] void fatal(const std::string &message);

}  // namespace toycc
