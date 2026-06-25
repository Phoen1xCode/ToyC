#pragma once

#include <memory>
#include <string>

#include "ast/ast.hpp"

namespace toycc::frontend {

std::unique_ptr<ast::CompUnit> parseSource(const std::string &source);

}  // namespace toycc::frontend
