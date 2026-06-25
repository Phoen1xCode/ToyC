#pragma once

#include "ast/ast.hpp"
#include "ir/ir.hpp"

namespace toycc::ir {

Program buildIr(const ast::CompUnit &program);

}  // namespace toycc::ir
