#include <iostream>
#include <iterator>
#include <string>

#include "backend/riscv.hpp"
#include "frontend/parser_driver.hpp"
#include "ir/ir_builder.hpp"
#include "ir/optim.hpp"
#include "sema/semantic.hpp"

namespace {

std::string readStdin() {
  return std::string(std::istreambuf_iterator<char>(std::cin),
                     std::istreambuf_iterator<char>());
}

}  // namespace

int main(int argc, char *argv[]) {
  const bool optimize = argc > 1 && std::string(argv[1]) == "-opt";
  (void)optimize;

  const std::string source = readStdin();
  auto ast = toycc::frontend::parseSource(source);
  toycc::sema::analyze(*ast);
  auto ir = toycc::ir::buildIr(*ast);

  // 本年度鼓励后端优化；这里的 IR 级优化在正确性路上之后总是启用。即使
  // 性能测试不传 `-opt`，也不影响功能正确性。
  toycc::ir::optimizeProgram(ir);

  toycc::backend::emitRiscv(ir, std::cout);
  return 0;
}
