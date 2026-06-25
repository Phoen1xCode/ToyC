#pragma once

#include <iosfwd>

#include "ir/ir.hpp"

namespace toycc::backend {

void emitRiscv(const ir::Program &program, std::ostream &out);

}  // namespace toycc::backend
