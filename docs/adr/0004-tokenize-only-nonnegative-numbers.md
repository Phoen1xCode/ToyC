# Tokenize only nonnegative numbers

The lexer will tokenize only nonnegative decimal integer literals, while negative numeric forms are represented by the parser as unary minus applied to a number expression. Although the assignment writes `NUMBER` with an optional leading minus, keeping `-` as an operator avoids ambiguity in expressions such as `a-1` and preserves the same ToyC source-level meaning.
