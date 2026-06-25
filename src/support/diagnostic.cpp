#include "support/diagnostic.hpp"

#include <cstdlib>
#include <iostream>

namespace toycc {

[[noreturn]] void fatal(const std::string &message) {
  std::cerr << "toycc: " << message << '\n';
  std::exit(1);
}

}  // namespace toycc
