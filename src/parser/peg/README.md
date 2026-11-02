# DuckDB PEG Parser Grammar

This document describes the PEG (Parsing Expression Grammar) system that DuckDB uses to parse SQL, including the grammar syntax, build process, and transformer code generation.

## Grammar Syntax

Grammar rules are defined in `.gram` files located in `src/parser/peg/grammar/statements/`. Each file typically corresponds to one SQL statement type (e.g., `select.gram`, `insert.gram`, `alter.gram`). Shared rules and macros live in `common.gram`.

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
DistinctOrAll <- DistinctKeyword / AllKeyword
LiteralExpression <- StringLiteral / NumberLiteral / ConstantLiteral
```

**Important**: Because PEG uses ordered choice, more specific alternatives must come before general ones. For example, compound interval types like `YEAR TO MONTH` must precede the simple `YEAR` keyword, otherwise the parser greedily consumes `YEAR` and fails on `TO MONTH`.

### Sequences

Elements listed one after another form a sequence — all must match in order:

```
CastExpression <- CastOrTryCast Parens(CastArguments)
ShowTables <- ShowOrDescribe 'TABLES' 'FROM' QualifiedName
```

### Optional (`?`)

The `?` operator makes an element optional (zero or one):

```
CreateStatement <- 'CREATE' OrReplace? Temporary? CreateStatementVariation
SelectClause <- 'SELECT' DistinctClause? TargetList?
```

### Repeat (`*` and `+`)

- `*` — zero or more occurrences
- `+` — one or more occurrences

```
IsExpression <- IsDistinctFromExpression IsTest*
SearchedCaseExpression <- 'CASE' CaseWhenThen+ CaseElse? 'END'
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

**Note**: Negative lookahead is parsed but currently ignored by the matcher (see the `FIXME` in `peg_parser.cpp`).
Restrict which keywords a rule accepts through the keyword lists and identifier matchers instead.

### Special Tokens

Some rules are handled by dedicated matchers rather than by grammar expansion. These are registered as terminal rule
overrides in `compiled_grammar.cpp` (between the `GENERATED RULE OVERRIDES` markers) and configured in the
`matcher_rule_overrides` section of `scripts/parser/grammar_types.yml`. They include:

- `Identifier` / `ReservedIdentifier` — generic identifier matching
- `CatalogName`, `SchemaName`, `TableName`, `ColumnName`, `IndexName`, `SequenceName`, `TableFunctionName`,
  `TypeName`, `PragmaName`, `SettingName`, `CopyOptionName` and their `Reserved*` variants — context-aware identifiers
- `ReservedKeyword` — any reserved keyword
- `StringLiteral` — single-quoted string literals
- `NumberLiteral` — numeric literals
- `OperatorLiteral` — operator symbols
- `EndOfInput` — end of the statement

### Comments

Lines starting with `#` are comments:

```
# Expression precedence levels
# LEVEL 1 (Lowest)
Expression <- LambdaArrowExpression
```

### Keywords

Keywords are defined in list files under `src/parser/peg/grammar/keywords/`:

| File | Description |
|------|-------------|
| `reserved_keyword.list` | Cannot be used as identifiers (e.g., `SELECT`, `FROM`) |
| `unreserved_keyword.list` | Can be used as identifiers (e.g., `ABORT`, `ABSOLUTE`) |
| `column_name_keyword.list` | Allowed as column names |
| `func_name_keyword.list` | Allowed as function names |
| `type_name_keyword.list` | Allowed as type names |
| `bare_label_keyword.list` | Allowed as a column alias without `AS` (e.g. `SELECT 1 name`) |

A keyword must appear in exactly one of the category lists; `inline_grammar.py` rejects violations.

`bare_label_keyword.list` is orthogonal to the categories, following Postgres' `bare_label_keyword`: every entry must
also appear in a category list, and only unreserved or column-name keywords listed there can be used as an `AS`-less
alias. Keywords that can follow an expression or type are deliberately left out (interval units such as `DAY`, function
suffixes such as `FILTER` and `OVER`, type suffixes such as `VARYING` and `WITHOUT`, and star modifiers such as
`EXCLUDE`), so `SELECT d + 1 day` is a syntax error rather than a column named `day`. New unreserved keywords should be
added to the list unless they can trail an expression.

## Building the Grammar

After modifying `.gram` files, keyword lists, or any of the generator configuration files, regenerate the grammar and
transformer artifacts:

```bash
make parser-grammar
```

This target installs the Python dependencies (PyYAML) into the formatting virtualenv and runs
`scripts/parser/build_grammar.sh`, which performs these steps in order:

1. `scripts/parser/inline_grammar.py --grammar-file` — combines all `.gram` files and keyword lists into
   `inlined_grammar.gram` (useful for debugging).
2. `scripts/parser/inline_grammar.py` — generates `inlined_grammar.hpp` (the grammar embedded as a byte array) and
   `keyword_map.cpp` (keyword lookup tables).
3. `scripts/parser/generate_transformer_trampoline.py --write` — generates the transformer processes, the rule IDs and
   the matcher configuration blocks (see below).
4. `make format-parser-grammar` — formats the generated C++ files.

The transformer generator can also be run directly with `--write` when only the transformer artifacts need
regenerating. Without `--write` it prints what would be generated instead of modifying files.

### Generated Files

| File | Generated by | Description |
|------|--------------|-------------|
| `src/include/duckdb/parser/peg/inlined_grammar.gram` | `inline_grammar.py` | Complete merged grammar (all `.gram` files + keywords) |
| `src/include/duckdb/parser/peg/inlined_grammar.hpp` | `inline_grammar.py` | C++ header: `const unsigned char INLINED_PEG_GRAMMAR[]` |
| `src/parser/peg/keyword_map.cpp` | `inline_grammar.py` | C++ keyword category lookup maps |
| `src/parser/peg/transformer/transform_generated_trampoline.cpp` | `generate_transformer_trampoline.py` | `Initialize*`/`Finalize*` transform processes and the frame-ops registration table |
| `src/include/duckdb/parser/peg/transformer/peg_transformer.hpp` | `generate_transformer_trampoline.py` | Declarations between the `GENERATED TRANSFORM RESULT TYPES` and `GENERATED TRAMPOLINE RULES` markers |
| `src/include/duckdb/parser/peg/peg_rule_id.hpp`, `src/parser/peg/peg_rule_id.cpp` | `generate_transformer_trampoline.py` | `PEGRuleId` enum with one constant per built-in rule, plus name lookups |
| `src/parser/peg/compiled_grammar.cpp` | `generate_transformer_trampoline.py` | Terminal rule overrides between the `GENERATED RULE OVERRIDES` markers |
| `src/parser/peg/matcher_factory.cpp` | `generate_transformer_trampoline.py` | Packrat-memoized rule list between the `GENERATED PACKRAT MEMOIZED RULES` markers |
| `src/parser/peg/transformer/peg_transformer_factory.cpp` | `generate_transformer_trampoline.py` | Collapsible and expression-depth rule lists between the `GENERATED COLLAPSIBLE RULES` and `GENERATED EXPRESSION DEPTH RULES` markers |

**Do not edit generated files or generated blocks manually** — they will be overwritten by the build script. Files that
are only partially generated contain `START GENERATED ... ` / `END GENERATED ...` marker comments around the
generated block; code outside the markers is hand-written.

## Implementing Transformer Rules

Each grammar rule that produces a value needs a transformer that converts its parse result into a DuckDB AST node
(`SQLStatement`, `QueryNode`, `ParsedExpression`, etc.) or an intermediate helper type.

Most of the parse-result plumbing is generated. For a grammar rule `Foo`, there are up to two pieces:

| Function | Written by | Purpose |
|----------|------------|---------|
| `InitializeFooTrampoline` / `FinalizeFooTrampoline` | generated | A two-phase transform process on the explicit transform stack: `Initialize` schedules the children, `Finalize` calls the typed body with their results and boxes the return value |
| `TransformFoo(transformer, typed children...)` | hand-written | The semantic body: builds the AST node from already-transformed children |

Rules that consist only of literals, or choice rules whose alternatives all return the same type, are generated end-to-end and need no hand-written body.

Generated transformer code refers to child rules by `PEGRuleId` rather than by name. Rules that grammar extensions
add at runtime receive IDs past `PEGRuleId::BUILTIN_COUNT` in the compiled grammar that defines them.

### Generator Configuration

`scripts/parser/grammar_types.yml` drives `generate_transformer_trampoline.py`:

- Type categories map grammar rules to their C++ return type (`unique_ptr<SQLStatement>`, `QualifiedName`, ...).
  Every rule that is referenced as a child of a generated rule must have a type here.
- `overrides` lists rules with a unique return type.
- `excluded_rules` lists syntax-only rules that are skipped when generating processes.
- `matcher_rule_overrides` lists the special tokens handled by dedicated matchers (see above).
- `packrat_memoized_rules`, `collapsible_rules`, `expression_depth_nesting_rules` and `expression_depth_chain_rules`
  configure the matcher and transformer: which rules are memoized, which transforms return their operand unchanged
  when the rest of the rule matched nothing, and which rules count towards `max_expression_depth`.

`scripts/parser/transformer_trampoline.yml` drives the per-rule process generation. Rules not listed there get a
generated process. Listed rules are either `excluded`, `forward` (delegate to a single child), or `manual` /
`manual_finalize` with hand-written `init` / `finalize` functions.

### Adding or Changing a Rule

1. Add or edit the rule in the relevant `.gram` file.
2. Add the rule's return type to `grammar_types.yml` (or `excluded_rules` if it is syntax only).
3. Run `make parser-grammar`. The generator prints the remaining manual steps, including the exact signature of any
   typed body that still needs to be hand-written.
4. Implement the body in the matching `transform_*.cpp` file (e.g. rules from `describe.gram` go in
   `transform_describe.cpp`), then run `make parser-grammar` again so the declarations pick it up.

### Transformer Implementation Pattern

The hand-written body receives already-transformed children in grammar order; literal tokens are skipped. For
`ShowTables <- ShowOrDescribe 'TABLES' 'FROM' QualifiedName`:

```cpp
unique_ptr<QueryNode> PEGTransformerFactory::TransformShowTables(PEGTransformer &transformer,
                                                                 const ShowType &show_or_describe,
                                                                 const QualifiedName &qualified_name) {
	auto showref = make_uniq<ShowRef>();
	showref->show_type = ShowType::SHOW_FROM;
	showref->SetSchemaName(qualified_name.Name());
	// ... build and return the AST node
}
```

The generated process that calls it looks like this (in `transform_generated_trampoline.cpp`):

```cpp
void PEGTransformerFactory::InitializeShowTablesTrampoline(PEGTransformer &transformer,
                                                           GeneratedTransformProcess &process) {
	auto &list_pr = process.parse_result.Cast<ListParseResult>();
	process.ReserveChildSlots(2);
	process.PushChild({transformer.GetRule(PEGRuleId::QualifiedName), list_pr.GetChild(3)}, 1);
	process.PushChild({transformer.GetRule(PEGRuleId::ShowOrDescribe), list_pr.GetChild(0)}, 0);
}

arena_ptr<TransformResultValue>
PEGTransformerFactory::FinalizeShowTablesTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process) {
	auto show_or_describe = process.TakeResult<ShowType>(0);
	auto qualified_name = process.TakeResult<QualifiedName>(1);
	auto result = TransformShowTables(transformer, show_or_describe, qualified_name);
	return transformer.MakeResult<unique_ptr<QueryNode>>(std::move(result));
}
```

Optional children arrive as `optional<T>` (or `unique_ptr<T>` that may be null) and repeated children as `vector<T>`.
Only the few rules that the generator cannot handle (listed under `excluded_rules`, or `manual` in the trampoline
config) still unpack the parse result by hand, using the types below.

### Parse Result Types

| Grammar Construct | Parse Result Type | Access Pattern |
|-------------------|-------------------|----------------|
| Sequence (`A B C`) | `ListParseResult` | `list_pr.Child<T>(index)`, `list_pr.GetChild(index)`, `GetChildren()` |
| Choice (`A / B`) | `ChoiceParseResult` | `choice_pr.GetResult()` (the matched alternative) |
| Optional (`A?`) | `OptionalParseResult` | `opt_pr.HasResult()`, `opt_pr.GetResult()` |
| Repeat (`A+` / `A*`) | `RepeatParseResult` | `repeat_pr.GetChildren()` |
| Identifier | `IdentifierParseResult` | `.identifier` (`Identifier`) |
| String literal | `StringLiteralParseResult` | `.result` (string), `.ToExpression()` (ParsedExpression) |
| Number literal | `NumberParseResult` | `.number` (string) |
| Keyword | `KeywordParseResult` | `.keyword` (string) |
| Operator | `OperatorParseResult` | `.operator_token` (string) |
| End of input | `EndOfInputParseResult` | — |

`PEGTransformerFactory::ExtractParseResultsFromList` and `ExtractResultFromParens` unwrap the `List(D)` and
`Parens(D)` macros when working with raw parse results.

### Registration

All transforms are registered through one table: `GeneratedTransformFrameOps()` in `transform_generated_trampoline.cpp`
maps every rule name to its `Initialize*`/`Finalize*` pair, and the `PEGTransformerFactory` constructor in
`peg_transformer_factory.cpp` walks that table, attaching the process to each rule together with its collapsible and
expression-depth configuration.

Rules whose process cannot be generated (`PivotStatement`, `TableRef`, `WithClause`, ...) are listed as `manual` or
`manual_finalize` in `transformer_trampoline.yml` with the names of their hand-written `init` / `finalize` functions.
Those functions live in the matching `transform_*.cpp` file and appear in the same table, so there is no separate
registration step for them.

## Extending the Grammar from Extensions

Extensions can modify the grammar at runtime without touching the `.gram` files. The relevant headers live in
`src/include/duckdb/parser/`.

### GrammarExtension and GrammarChange

A `GrammarExtension` (`grammar_extension.hpp`) is a named, composable set of `GrammarChange`s
(`grammar_change.hpp`) that is applied on top of the built-in grammar when the parser is compiled
(`CompiledGrammar::Create` in `compiled_grammar.cpp`). Available changes:

| Change | Effect |
|--------|--------|
| `AddRule(definition, transform_process)` | Add a new rule, optionally with a transform process for it |
| `ReplaceRule(definition, transform_process)` | Replace an existing rule |
| `AddChoice(rule, choice)` / `PrependChoice(rule, choice)` | Append or prepend an alternative to a choice rule |
| `RemoveChoice(rule, find_cursor)` / `ReplaceChoice(rule, choice, find_cursor)` | Remove or replace an alternative |
| `SetTransformProcess(rule, transform_process)` | Attach a transform process to an existing rule |
| `AddTerminalRuleOverride(rule, matcher_factory)` | Replace a rule with a custom `Matcher` |

Choices can reference built-in rules, so `GrammarChange::AddChoice("UnreservedKeyword", "'ANSWER'")` adds a keyword and
`GrammarChange::PrependChoice("SelectAtom", "PipeSelectAtom")` hooks a new statement form into `SELECT`.

Register the extension with `GrammarExtension::Register(db, make_shared_ptr<MyGrammarExtension>())`, usually from the
extension's load function. Registered extensions are listed by `duckdb_grammar_extensions()` and are inactive by
default; a connection enables them with:

```sql
SET active_grammar_extensions = ['pipe_query_syntax'];
```

Transforms for added rules are written against the raw parse-result types described above, since the generators only
run over the in-tree grammar. See `test/extension/loadable_grammar_extension_demo.cpp` for a complete example that adds
pipe syntax (`FROM tbl |> WHERE ... |> SELECT ...`), and `test/api/test_grammar_extension.cpp` for a minimal one that
also adds a custom matcher.

### DialectExtension

A `DialectExtension` (`dialect_extension.hpp`) is a named SQL dialect, registered with
`DialectExtension::Register(config, make_shared_ptr<MyDialect>())`. Registered dialects are listed by
`duckdb_dialects()` and selected per connection with `SET current_dialect = 'name'`; the built-in dialect is `duckdb`.
A dialect subclass overrides `ApplyGrammarChanges` to add, replace or splice rules into the base grammar (the same
operations as a `GrammarExtension`), and can additionally replace the tokenizer, the keyword helper and the matcher
factory through `CreateTokenizer`, `CreateKeywordHelper` and `CreateMatcherFactory`. See
`test/extension/loadable_dialect_extension_demo.cpp` for a loadable example.
