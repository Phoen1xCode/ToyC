#include "frontend/parser_driver.hpp"

#include <cstddef>
#include <memory>
#include <string>

#include "ast/ast.hpp"
#include "support/diagnostic.hpp"

struct yy_buffer_state;
using YY_BUFFER_STATE = yy_buffer_state *;

extern int yyparse(void);
extern YY_BUFFER_STATE yy_scan_bytes(const char *bytes, std::size_t len);
extern void yy_delete_buffer(YY_BUFFER_STATE buffer);
extern int yylex_destroy(void);

namespace toycc::frontend {

extern std::unique_ptr<ast::CompUnit> g_parseResult;

std::unique_ptr<ast::CompUnit> parseSource(const std::string &source) {
  g_parseResult.reset();
  YY_BUFFER_STATE buffer = yy_scan_bytes(source.data(), static_cast<int>(source.size()));
  const int result = yyparse();
  yy_delete_buffer(buffer);
  yylex_destroy();
  if (result != 0 || !g_parseResult) fatal("failed to parse source");
  return std::move(g_parseResult);
}

}  // namespace toycc::frontend
