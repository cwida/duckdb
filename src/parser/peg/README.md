# DuckDB PEG Parser Grammar

This document describes the PEG (Parsing Expression Grammar) system used by DuckDB's autocomplete extension, including the grammar syntax, build process, and transformer code generation.

## Grammar Syntax

Grammar rules are defined in `.gram` files located in `extension/autocomplete/grammar/statements/`. Each file typically corresponds to one SQL statement type (e.g., `select.gram`, `insert.gram`, `alter.gram`).

### Rule Definitions

Rules are defined with the `<-` operator:

```
RuleName <- RuleDefinition
```

### Literals

Keywords and operators are matched with single-quoted strings:

```
'CREATE'  'TABLE'  'SELECT'  ','  '('  ')'
```

Matching is case-insensitive by default for keyword literals.

### Choices (Ordered Alternatives)

The `/` operator tries alternatives left-to-right. The first successful match wins (PEG semantics — no ambiguity):

```
DistinctOrAll <- 'DISTINCT' / 'ALL'
LiteralExpression <- StringLiteral / NumberLiteral / ConstantLiteral
```

**Important**: Because PEG uses ordered choice, more specific alternatives must come before general ones. For example, compound interval types like `YEAR TO MONTH` must precede the simple `YEAR` keyword, otherwise the parser greedily consumes `YEAR` and fails on `TO MONTH`.

### Sequences

Elements listed one after another form a sequence — all must match in order:

```
CastExpression <- CastOrTryCast Parens(Expression 'AS' Type)
UpdateSetClause <- 'SET' List(UpdateSetExpression)
```

### Optional (`?`)

The `?` operator makes an element optional (zero or one):

```
CreateStatement <- 'CREATE' OrReplace? Temporary? CreateStatementVariation
SelectClause <- 'SELECT' DistinctClause? TargetList
```

### Repeat (`*` and `+`)

- `*` — zero or more occurrences
- `+` — one or more occurrences

```
LogicalNotExpression <- 'NOT'* IsExpression
CaseExpression <- 'CASE' Expression? CaseWhenThen+ CaseElse? 'END'
```

### Grouping with Parentheses

Parentheses group sub-expressions without creating a named rule:

```
Interval <- (YearKeyword 'TO' MonthKeyword) /
    (DayKeyword 'TO' HourKeyword) /
    YearKeyword /
    MonthKeyword
```

### Parameterized Rules (Macros)

Rules can accept parameters, acting as reusable templates. Two built-in macros are defined in `common.gram`:

```
List(D) <- D (',' D)* ','?
Parens(D) <- '(' D ')'
```

Usage:

```
Parens(List(Expression))                        -- (expr, expr, ...)
Parens(DistinctOrAll? List(FunctionArgument)?)   -- Nesting allowed
List(BaseTableName)                              -- table1, table2, ...
```

### Negative Lookahead (`!`)

The `!` operator asserts that a pattern does NOT match at the current position (without consuming input):

```
PlainIdentifier <- !ReservedKeyword <[a-z_]i[a-z0-9_]i*>
```

**Note**: Negative lookahead is parsed but currently ignored by the matcher.
TODO(Dtenwolde): Implement negative lookahead

### Special Tokens

Some rules are handled directly by the matcher rather than by grammar expansion. These include:

- `Identifier` / `ReservedIdentifier` — variable/identifier matching
- `TableName`, `ColumnName`, `SchemaName`, etc. — context-aware identifier matching
- `StringLiteral` — single-quoted string literals
- `NumberLiteral` — numeric literals
- `OperatorLiteral` — operator symbols

See `matcher.cpp` for a full list. 

### Comments

Lines starting with `#` are comments:

```
# Expression precedence levels
# LEVEL 1 (Lowest)
Expression <- LogicalOrExpression
```

### Keywords

Keywords are defined in list files under `grammar/keywords/`:

| File | Description |
|------|-------------|
| `reserved_keyword.list` | Cannot be used as identifiers (e.g., `SELECT`, `FROM`) |
| `unreserved_keyword.list` | Can be used as identifiers (e.g., `ABORT`, `ABSOLUTE`) |
| `column_name_keyword.list` | Allowed as column names |
| `func_name_keyword.list` | Allowed as function names |
| `type_name_keyword.list` | Allowed as type names |
| `bare_label_keyword.list` | Allowed as a column alias without `AS` (e.g. `SELECT 1 name`) |

A keyword must appear in exactly one of the category lists. The build script validates this.

`bare_label_keyword.list` is orthogonal to the categories, following Postgres' `bare_label_keyword`: every entry must
also appear in a category list, and only unreserved or column-name keywords listed there can be used as an `AS`-less
alias. Keywords that can follow an expression or type are deliberately left out (interval units such as `DAY`, function
suffixes such as `FILTER` and `OVER`, type suffixes such as `VARYING` and `WITHOUT`, and star modifiers such as
`EXCLUDE`), so `SELECT d + 1 day` is a syntax error rather than a column named `day`. New unreserved keywords should be
added to the list unless they can trail an expression.

## Building the Grammar

After modifying `.gram` files or keyword lists, regenerate the inlined grammar files:

```bash
./scripts/parser/build_grammar.sh
```

This script runs `extension/autocomplete/inline_grammar.py` twice:

1. **`--grammar-file`**: Combines all `.gram` files and keyword lists into a single `inlined_grammar.gram` (useful for debugging).
2. **Default**: Generates `inlined_grammar.hpp` (C++ header embedding the grammar as a string constant) and `keyword_map.cpp` (keyword lookup tables).

### Generated Files

| File | Description |
|------|-------------|
| `include/inlined_grammar.gram` | Complete merged grammar (all `.gram` files + keywords) |
| `include/inlined_grammar.hpp` | C++ header: `const char INLINED_PEG_GRAMMAR[]` |
| `keyword_map.cpp` | C++ keyword category lookup maps |

**Do not edit generated files manually** — they will be overwritten by the build script.

## Implementing Transformer Rules

Each grammar rule that produces a parse result needs a corresponding transformer function to convert it into a DuckDB AST node (`SQLStatement`, `QueryNode`, `ParsedExpression`, etc.).

### Generating Wrappers

Run the grammar build script to regenerate parser artifacts and typed transformer processes:

```bash
./scripts/parser/build_grammar.sh
```

The transformer generator (`scripts/parser/generate_transformer_trampoline.py`) reads
`scripts/parser/grammar_types.yml` to map grammar rules to C++ return types. For supported rules, it updates:

| File | Generated Content |
|------|-------------------|
| `peg_transformer.hpp` | Transformer process declarations and transform result types |
| `transform_generated_trampoline.cpp` | Transformer process implementations and registration table |
| `matcher.cpp` | Packrat-memoized matcher rules |
| `compiled_grammar.cpp` | Matcher rule overrides |
| `include/peg_rule_id.hpp`, `peg_rule_id.cpp` | `PEGRuleId` enum with one constant per built-in rule, plus name lookups |

If a generated process needs a hand-written semantic body, add that implementation to the appropriate
`transform_*.cpp` file, then rerun `build_grammar.sh`.

Run `scripts/parser/generate_transformer_trampoline.py --write` directly only when regenerating transformer
processes without rebuilding the inlined grammar.

Generated transformer code refers to child rules by `PEGRuleId` rather than by name. Rules that grammar extensions
add at runtime receive IDs past `PEGRuleId::BUILTIN_COUNT` in the compiled grammar that defines them.

### Transformer Implementation Pattern

A typical transformer function:

1. Casts the parse result to the expected type (usually `ListParseResult` for sequences, `ChoiceParseResult` for alternatives)
2. Extracts children by index (matching the grammar rule's element order)
3. Schedules child transformations on the explicit transform stack
4. Constructs and returns the appropriate AST node

Example for `ShowTables <- ShowOrDescribe 'TABLES' 'FROM' QualifiedName`:

```cpp
unique_ptr<QueryNode> PEGTransformerFactory::TransformShowTables(PEGTransformer &transformer,
                                                                  optional_ptr<ParseResult> parse_result) {
    auto &list_pr = parse_result->Cast<ListParseResult>();
    // Child 0: ShowOrDescribe, Child 1: 'TABLES', Child 2: 'FROM', Child 3: QualifiedName
    auto qualified_name = transformer.Transform<QualifiedName>(list_pr.Child<ListParseResult>(3));
    auto showref = make_uniq<ShowRef>();
    showref->show_type = ShowType::SHOW_FROM;
    showref->SetSchemaName(qualified_name.Name());
    // ... build and return the AST node
}
```

### Parse Result Types

| Grammar Construct | Parse Result Type | Access Pattern |
|-------------------|-------------------|----------------|
| Sequence (`A B C`) | `ListParseResult` | `list_pr.Child<T>(index)`, `list_pr.GetChild(index)` |
| Choice (`A / B`) | `ChoiceParseResult` | `choice_pr.result` (the matched alternative) |
| Optional (`A?`) | `OptionalParseResult` | `opt_pr.HasResult()`, `opt_pr.optional_result` |
| Repeat (`A+` / `A*`) | `RepeatParseResult` | Iterable list of results |
| Identifier | `IdentifierParseResult` | `.result` (string) |
| String literal | `StringLiteralParseResult` | `.result` (string), `.ToExpression()` (ParsedExpression) |
| Number literal | `NumberParseResult` | `.result` (string) |
| Keyword | `KeywordParseResult` | `.result` (string) |

### Registration

Transforms are registered in category-specific methods in `peg_transformer_factory.cpp`:

```cpp
void PEGTransformerFactory::RegisterDescribe() {
    REGISTER_TRANSFORM(TransformDescribeStatement);
    REGISTER_TRANSFORM(TransformShowTables);
    REGISTER_TRANSFORM(TransformShowAllTables);
    // ...
}
```

The `REGISTER_TRANSFORM` macro strips the `Transform` prefix to derive the grammar rule name, so `TransformShowTables` registers for the `ShowTables` grammar rule.
