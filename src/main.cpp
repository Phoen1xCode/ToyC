#include <iostream>
#include <iterator>
#include <string>

#include "backend/riscv.hpp"
#include "frontend/parser_driver.hpp"
#include "ir/ir_builder.hpp"
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

  toycc::backend::emitRiscv(ir, std::cout);
  return 0;
}
