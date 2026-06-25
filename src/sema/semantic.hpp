#pragma once

#include "ast/ast.hpp"

namespace toycc::sema {

void analyze(const ast::CompUnit &program);

}  // namespace toycc::sema
