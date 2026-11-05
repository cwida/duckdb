import re
from dataclasses import dataclass, field
from enum import Enum, auto
from typing import List

from inline_grammar import PEGTokenType


# ---------------------------------------------------------------------------
# Grammar AST - mirrors the Matcher class hierarchy in matcher.cpp
# ---------------------------------------------------------------------------


class GrammarNode:
    pass


@dataclass
class LiteralNode(GrammarNode):
    """Keyword literal ('keyword'). Corresponds to KeywordMatcher."""

    text: str


@dataclass
class ReferenceNode(GrammarNode):
    """Reference to a named grammar rule. Resolved to another Matcher at build time."""

    name: str


@dataclass
class ParensNode(GrammarNode):
    """Parens(D) <- '(' D ')'. Anonymous ListMatcher; child[1] is D's result."""

    inner: GrammarNode


@dataclass
class ListMacroNode(GrammarNode):
    """List(D) <- D (',' D)* ','?. Anonymous ListMatcher."""

    inner: GrammarNode


@dataclass
class FunctionCallNode(GrammarNode):
    """Unknown macro call (not Parens or List). Not auto-generated."""

    func_name: str
    inner: GrammarNode


@dataclass
class SequenceNode(GrammarNode):
    """Ordered sequence of matchers. Corresponds to ListMatcher."""

    children: List[GrammarNode]


@dataclass
class ChoiceNode(GrammarNode):
    """Ordered choice A / B / C. Corresponds to ChoiceMatcher."""

    alternatives: List[GrammarNode]


@dataclass
class OptionalNode(GrammarNode):
    """Optional match A?. Corresponds to OptionalMatcher."""

    child: GrammarNode


@dataclass
class RepeatNode(GrammarNode):
    """Repeat match A+ (one or more). A* is represented as OptionalNode(RepeatNode)."""

    child: GrammarNode


def tokens_to_ast(tokens):
    """
    Parse a flat PEGToken list into a GrammarNode AST tree.

    Grammar of PEG rule bodies (simplified):
      choice   = sequence ('/' sequence)*
      sequence = term+
      term     = atom ('?' | '*' | '+')?
      atom     = LITERAL | REFERENCE
                 | FUNCTION_CALL choice ')'
                 | '(' choice ')'
    """
    pos = [0]

    def peek():
        return tokens[pos[0]] if pos[0] < len(tokens) else None

    def consume():
        tok = tokens[pos[0]]
        pos[0] += 1
        return tok

    def parse_choice():
        alts = [parse_sequence()]
        while peek() and peek().type == PEGTokenType.OPERATOR and peek().text == "/":
            consume()
            alts.append(parse_sequence())
        return ChoiceNode(alts) if len(alts) > 1 else alts[0]

    def parse_sequence():
        children = []
        while True:
            token = peek()
            if token is None:
                break
            if token.type == PEGTokenType.OPERATOR and token.text in ("/", ")"):
                break
            children.append(parse_term())
        if not children:
            return SequenceNode([])
        return SequenceNode(children) if len(children) > 1 else children[0]

    def parse_term():
        node = parse_atom()
        token = peek()
        if token and token.type == PEGTokenType.OPERATOR and token.text in ("?", "*", "+"):
            op = consume().text
            if op == "?":
                return OptionalNode(node)
            if op == "*":
                return OptionalNode(RepeatNode(node))
            if op == "+":
                return RepeatNode(node)
            raise Exception("Unknown operator '{}'".format(op))
        return node

    def parse_atom():
        token = peek()
        if token is None:
            raise Exception("Unexpected end of tokens in grammar AST parse")
        if token.type == PEGTokenType.LITERAL:
            return LiteralNode(consume().text)
        if token.type == PEGTokenType.REFERENCE:
            return ReferenceNode(consume().text)
        if token.type == PEGTokenType.FUNCTION_CALL:
            func_name = consume().text
            inner = parse_choice()
            if peek() and peek().type == PEGTokenType.OPERATOR and peek().text == ")":
                consume()
            if func_name == "Parens":
                return ParensNode(inner)
            if func_name == "List":
                return ListMacroNode(inner)
            return FunctionCallNode(func_name, inner)
        if token.type == PEGTokenType.OPERATOR and token.text == "(":
            consume()
            inner = parse_choice()
            if peek() and peek().type == PEGTokenType.OPERATOR and peek().text == ")":
                consume()
            return inner
        raise Exception(f"Unexpected token in grammar AST parse: {token}")

    result = parse_choice()
    if pos[0] < len(tokens):
        raise Exception(f"Tokens remaining after grammar AST parse: {tokens[pos[0]:]}")
    return result


def rule_to_ast(rule):
    """Convert a PEGGrammarRule (flat token list) to a GrammarNode AST."""
    return tokens_to_ast(rule.tokens)


def to_snake_case(name):
    s1 = re.sub(r"(.)([A-Z][a-z]+)", r"\1_\2", name)
    return re.sub(r"([a-z0-9])([A-Z])", r"\1_\2", s1).lower()


def is_pure_reference_choice(ast):
    """True if ast is a ChoiceNode whose every alternative is a ReferenceNode."""
    return isinstance(ast, ChoiceNode) and all(
        isinstance(alternative, ReferenceNode) for alternative in ast.alternatives
    )


def literal_string_values(ast):
    """Return literal alternatives for a string-valued literal rule, or None if the rule is not literal-only."""
    if isinstance(ast, LiteralNode):
        return [ast.text]
    if isinstance(ast, ChoiceNode) and all(isinstance(alternative, LiteralNode) for alternative in ast.alternatives):
        return [alternative.text for alternative in ast.alternatives]
    return None


def classify_choice_alternatives(alternatives, rule_types, excluded_rules, identifier_override_rules):
    """
    Split choice alternatives into semantic groups.
    Returns (transformer_alts, identifier_alts, excluded_alts, unknown_alts).
    """
    transformer_alts = []
    identifier_alts = []
    excluded_alts = []
    unknown_alts = []
    for ref in alternatives:
        assert isinstance(ref, ReferenceNode)
        name = ref.name
        if name in identifier_override_rules:
            identifier_alts.append(name)
        elif name in rule_types:
            transformer_alts.append(name)
        elif name in excluded_rules:
            excluded_alts.append(name)
        else:
            unknown_alts.append(name)
    return transformer_alts, identifier_alts, excluded_alts, unknown_alts


# ---------------------------------------------------------------------------
# TransformProcess sequence planning
# ---------------------------------------------------------------------------


@dataclass
class MatcherOverride:
    rule_name: str
    matcher: str

    def is_identifier(self):
        return self.matcher in ("identifier", "reserved_identifier")

    def is_terminal(self):
        return self.matcher in (
            "number_literal",
            "string_literal",
            "operator",
            "all_operators",
            "identifier_string",
        )


@dataclass
class DirectParseArg:
    var_name: str
    parse_expr: str


@dataclass
class DirectMatcherArg:
    parse_expr: str
    rule_name: str
    var_name: str


@dataclass
class DirectRepeatArg:
    parse_expr: str
    rule_name: str
    var_name: str


@dataclass
class DirectListArg:
    parse_expr: str
    rule_name: str
    var_name: str


@dataclass
class DirectOptionalListArg:
    parse_expr: str
    rule_name: str
    var_name: str
    result_expr_template: str = "{opt}.GetResult()"


@dataclass
class DirectOptionalArg:
    parse_expr: str
    rule_name: str
    var_name: str


@dataclass
class DirectOptionalMatcherArg:
    parse_expr: str
    rule_name: str
    var_name: str
    result_expr_template: str = "{opt}.GetResult()"


@dataclass
class DirectOptionalPresenceArg:
    parse_expr: str
    var_name: str


@dataclass
class StackChild:
    parse_expr: str
    rule_name: str
    slot_idx: int
    var_name: str


@dataclass
class OptionalStackChild:
    parse_expr: str
    rule_name: str
    slot_idx: int
    var_name: str
    result_expr_template: str = "{opt}.GetResult()"


@dataclass
class TrailingOptionalStackChild:
    parse_expr: str
    rule_name: str
    var_name: str
    result_expr_template: str = "{opt}.GetResult()"


@dataclass
class RepeatStackChild:
    parse_expr: str
    rule_name: str
    slot_start: int
    var_name: str


@dataclass
class OptionalListStackChild:
    parse_expr: str
    rule_name: str
    slot_start: int
    var_name: str
    result_expr_template: str = "{opt}.GetResult()"


@dataclass
class RequiredRepeatStackChild:
    parse_expr: str
    rule_name: str
    slot_start: int
    var_name: str


@dataclass
class ListStackChild:
    parse_expr: str
    rule_name: str
    slot_start: int
    var_name: str


@dataclass
class SequenceRulePlan:
    direct_args: List[DirectParseArg]
    direct_optional_args: List[DirectOptionalArg]
    direct_optional_presence_args: List[DirectOptionalPresenceArg]
    direct_repeat_args: List[DirectRepeatArg]
    direct_list_args: List[DirectListArg]
    direct_optional_list_args: List[DirectOptionalListArg]
    stack_children: List[StackChild]
    optional_stack_children: List[OptionalStackChild]
    repeat_child: "RepeatStackChild | None"
    optional_list_child: "OptionalListStackChild | None"
    required_repeat_child: "RequiredRepeatStackChild | None"
    list_child: "ListStackChild | None"
    trailing_optional_stack_child: "TrailingOptionalStackChild | None"
    finalize_args: List[object]


def _trampoline_parse_expr(child_idx, child):
    parse_expr = f"list_pr.GetChild({child_idx})"
    while isinstance(child, ParensNode):
        parse_expr = f"ExtractResultFromParens({parse_expr})"
        child = child.inner
    return parse_expr, child


def _single_semantic_child_plan(node, source_expr_template, syntax_only_rules):
    while isinstance(node, ParensNode):
        source_expr_template = f"ExtractResultFromParens({source_expr_template})"
        node = node.inner
    if isinstance(node, ReferenceNode):
        return node.name, source_expr_template
    if isinstance(node, SequenceNode):
        semantic = []
        for child_idx, child in enumerate(node.children):
            child_expr_template, child = _sequence_child_expr_template(source_expr_template, child_idx, child)
            if isinstance(child, LiteralNode) or is_syntax_only_child(child, syntax_only_rules):
                continue
            if isinstance(child, ReferenceNode):
                semantic.append((child.name, child_expr_template))
                continue
            return None
        if len(semantic) == 1:
            return semantic[0]
    return None


def _sequence_child_expr_template(source_expr_template, child_idx, child):
    child_expr_template = f"{source_expr_template}.Cast<ListParseResult>().GetChild({child_idx})"
    while isinstance(child, ParensNode):
        child_expr_template = f"ExtractResultFromParens({child_expr_template})"
        child = child.inner
    return child_expr_template, child


def is_syntax_only_rule_body(node, syntax_only_rules):
    """Return true when an entire untyped rule has no semantic value."""
    if literal_string_values(node) is not None:
        return True
    if isinstance(node, ReferenceNode):
        return node.name in syntax_only_rules
    if isinstance(node, OptionalNode):
        return is_syntax_only_rule_body(node.child, syntax_only_rules)
    if isinstance(node, RepeatNode):
        return is_syntax_only_rule_body(node.child, syntax_only_rules)
    if isinstance(node, ChoiceNode):
        return all(is_syntax_only_rule_body(alternative, syntax_only_rules) for alternative in node.alternatives)
    if isinstance(node, SequenceNode):
        return all(is_syntax_only_rule_body(child, syntax_only_rules) for child in node.children)
    if isinstance(node, ParensNode):
        return is_syntax_only_rule_body(node.inner, syntax_only_rules)
    return False


def is_syntax_only_child(node, syntax_only_rules):
    """Return true when a sequence child can be ignored without losing presence information."""
    if literal_string_values(node) is not None:
        return True
    if isinstance(node, ReferenceNode):
        return node.name in syntax_only_rules
    if isinstance(node, ChoiceNode):
        return all(is_syntax_only_child(alternative, syntax_only_rules) for alternative in node.alternatives)
    if isinstance(node, SequenceNode):
        return all(is_syntax_only_child(child, syntax_only_rules) for child in node.children)
    if isinstance(node, ParensNode):
        return is_syntax_only_child(node.inner, syntax_only_rules)
    return False


def _unwrap_parens_node(node, source_expr_template):
    while isinstance(node, ParensNode):
        source_expr_template = f"ExtractResultFromParens({source_expr_template})"
        node = node.inner
    return node, source_expr_template


def _is_identifier_override(rule_name, matcher_overrides):
    override = matcher_overrides.get(rule_name)
    return override is not None and override.is_identifier()


def _is_terminal_override(rule_name, matcher_overrides):
    override = matcher_overrides.get(rule_name)
    return override is not None and override.is_terminal()


def plan_trampoline_sequence_rule(ast, matcher_overrides, syntax_only_rules=None):
    return plan_trampoline_sequence_rule_with_terminals(ast, matcher_overrides, syntax_only_rules)


def plan_trampoline_sequence_rule_with_terminals(
    ast,
    matcher_overrides,
    syntax_only_rules=None,
    allow_dynamic_stack_followers=False,
    allow_top_level_repeat=False,
):
    syntax_only_rules = syntax_only_rules or set()
    direct_args = []
    direct_optional_args = []
    direct_optional_presence_args = []
    direct_repeat_args = []
    direct_list_args = []
    direct_optional_list_args = []
    stack_children = []
    optional_stack_children = []
    repeat_child = None
    optional_list_child = None
    required_repeat_child = None
    list_child = None
    trailing_optional_stack_child = None
    finalize_args = []
    next_slot = 0
    children = ast.children if isinstance(ast, SequenceNode) else [ast]
    for child_idx, child in enumerate(children):
        parse_expr, child = _trampoline_parse_expr(child_idx, child)
        has_dynamic_child = (
            list_child is not None
            or repeat_child is not None
            or optional_list_child is not None
            or required_repeat_child is not None
        )
        if isinstance(child, LiteralNode) or is_syntax_only_child(child, syntax_only_rules):
            continue
        if isinstance(child, OptionalNode) and is_syntax_only_child(child.child, syntax_only_rules):
            var_name = "has_result" if len(direct_optional_presence_args) == 0 else f"has_result_{child_idx}"
            arg = DirectOptionalPresenceArg(parse_expr, var_name)
            direct_optional_presence_args.append(arg)
            finalize_args.append(arg)
            continue
        if isinstance(child, ReferenceNode):
            if _is_identifier_override(child.name, matcher_overrides):
                arg = DirectParseArg(
                    to_snake_case(child.name),
                    f"{parse_expr}.Cast<IdentifierParseResult>().identifier",
                )
                direct_args.append(arg)
            elif _is_terminal_override(child.name, matcher_overrides):
                arg = DirectMatcherArg(parse_expr, child.name, to_snake_case(child.name))
                direct_args.append(arg)
            else:
                if has_dynamic_child and not allow_dynamic_stack_followers:
                    raise NotImplementedError("stack child after dynamic stack child is currently unsupported")
                arg = StackChild(parse_expr, child.name, next_slot, to_snake_case(child.name))
                stack_children.append(arg)
                next_slot += 1
            finalize_args.append(arg)
            continue
        if isinstance(child, OptionalNode) and isinstance(child.child, ReferenceNode):
            if _is_identifier_override(child.child.name, matcher_overrides):
                arg = DirectOptionalArg(parse_expr, child.child.name, to_snake_case(child.child.name))
                direct_optional_args.append(arg)
            elif _is_terminal_override(child.child.name, matcher_overrides):
                arg = DirectOptionalMatcherArg(parse_expr, child.child.name, to_snake_case(child.child.name))
            else:
                if has_dynamic_child:
                    if not allow_dynamic_stack_followers:
                        if trailing_optional_stack_child is not None:
                            raise NotImplementedError("only one trailing optional stack child is currently supported")
                        arg = TrailingOptionalStackChild(
                            parse_expr,
                            child.child.name,
                            to_snake_case(child.child.name),
                        )
                        trailing_optional_stack_child = arg
                        finalize_args.append(arg)
                        continue
                    arg = OptionalStackChild(
                        parse_expr,
                        child.child.name,
                        next_slot,
                        to_snake_case(child.child.name),
                    )
                    optional_stack_children.append(arg)
                    next_slot += 1
                else:
                    arg = OptionalStackChild(
                        parse_expr,
                        child.child.name,
                        next_slot,
                        to_snake_case(child.child.name),
                    )
                    optional_stack_children.append(arg)
                    next_slot += 1
            finalize_args.append(arg)
            continue
        optional_stack_plan = None
        if isinstance(child, OptionalNode):
            optional_stack_plan = _single_semantic_child_plan(child.child, "{opt}.GetResult()", syntax_only_rules)
        if optional_stack_plan is not None:
            rule_name, result_expr_template = optional_stack_plan
            if _is_identifier_override(rule_name, matcher_overrides):
                raise NotImplementedError("wrapped optional identifier child is currently unsupported")
            if _is_terminal_override(rule_name, matcher_overrides):
                arg = DirectOptionalMatcherArg(
                    parse_expr,
                    rule_name,
                    to_snake_case(rule_name),
                    result_expr_template,
                )
                finalize_args.append(arg)
                continue
            if has_dynamic_child:
                if not allow_dynamic_stack_followers:
                    if trailing_optional_stack_child is not None:
                        raise NotImplementedError("only one trailing optional stack child is currently supported")
                    arg = TrailingOptionalStackChild(
                        parse_expr,
                        rule_name,
                        to_snake_case(rule_name),
                        result_expr_template,
                    )
                    trailing_optional_stack_child = arg
                    finalize_args.append(arg)
                    continue
                arg = OptionalStackChild(
                    parse_expr,
                    rule_name,
                    next_slot,
                    to_snake_case(rule_name),
                    result_expr_template,
                )
                optional_stack_children.append(arg)
                next_slot += 1
            else:
                arg = OptionalStackChild(
                    parse_expr,
                    rule_name,
                    next_slot,
                    to_snake_case(rule_name),
                    result_expr_template,
                )
                optional_stack_children.append(arg)
                next_slot += 1
            finalize_args.append(arg)
            continue
        transparent_child_plan = _single_semantic_child_plan(child, parse_expr, syntax_only_rules)
        if transparent_child_plan is not None and not isinstance(child, ReferenceNode):
            rule_name, child_parse_expr = transparent_child_plan
            if _is_identifier_override(rule_name, matcher_overrides):
                arg = DirectParseArg(
                    to_snake_case(rule_name),
                    f"{child_parse_expr}.Cast<IdentifierParseResult>().identifier",
                )
                direct_args.append(arg)
            elif _is_terminal_override(rule_name, matcher_overrides):
                arg = DirectMatcherArg(child_parse_expr, rule_name, to_snake_case(rule_name))
                direct_args.append(arg)
            else:
                if has_dynamic_child and not allow_dynamic_stack_followers:
                    raise NotImplementedError("stack child after dynamic stack child is currently unsupported")
                arg = StackChild(child_parse_expr, rule_name, next_slot, to_snake_case(rule_name))
                stack_children.append(arg)
                next_slot += 1
            finalize_args.append(arg)
            continue
        if isinstance(child, OptionalNode) and isinstance(child.child, RepeatNode):
            repeat_node = child.child.child
            if isinstance(repeat_node, ReferenceNode):
                if _is_identifier_override(repeat_node.name, matcher_overrides):
                    arg = DirectRepeatArg(parse_expr, repeat_node.name, to_snake_case(repeat_node.name))
                    direct_repeat_args.append(arg)
                    finalize_args.append(arg)
                    continue
                if _is_terminal_override(repeat_node.name, matcher_overrides):
                    raise NotImplementedError("terminal override repeat is currently unsupported")
                if repeat_child is not None:
                    raise NotImplementedError("only one repeat child is currently supported")
                repeat_child = RepeatStackChild(
                    parse_expr,
                    repeat_node.name,
                    next_slot,
                    to_snake_case(repeat_node.name),
                )
                next_slot += 1
                finalize_args.append(repeat_child)
                continue
        if isinstance(child, OptionalNode) and isinstance(child.child, ListMacroNode):
            list_node = child.child.inner
            result_expr_template = "{opt}.GetResult()"
            if isinstance(child.child, ParensNode):
                list_node = child.child.inner
                result_expr_template = "ExtractResultFromParens({opt}.GetResult())"
            if isinstance(list_node, ReferenceNode):
                if _is_identifier_override(list_node.name, matcher_overrides):
                    arg = DirectOptionalListArg(
                        parse_expr,
                        list_node.name,
                        to_snake_case(list_node.name),
                        result_expr_template,
                    )
                    direct_optional_list_args.append(arg)
                    finalize_args.append(arg)
                    continue
                if _is_terminal_override(list_node.name, matcher_overrides):
                    arg = DirectOptionalListArg(
                        parse_expr,
                        list_node.name,
                        to_snake_case(list_node.name),
                        result_expr_template,
                    )
                    direct_optional_list_args.append(arg)
                    finalize_args.append(arg)
                    continue
                if (
                    list_child is not None
                    or repeat_child is not None
                    or optional_list_child is not None
                    or required_repeat_child is not None
                ):
                    raise NotImplementedError("only one dynamic stack child is currently supported")
                optional_list_child = OptionalListStackChild(
                    parse_expr,
                    list_node.name,
                    next_slot,
                    to_snake_case(list_node.name),
                    result_expr_template,
                )
                next_slot += 1
                finalize_args.append(optional_list_child)
                continue
        if isinstance(child, OptionalNode):
            list_container, result_expr_template = _unwrap_parens_node(child.child, "{opt}.GetResult()")
            if isinstance(list_container, ListMacroNode) and isinstance(list_container.inner, ReferenceNode):
                list_node = list_container.inner
                if _is_identifier_override(list_node.name, matcher_overrides):
                    arg = DirectOptionalListArg(
                        parse_expr,
                        list_node.name,
                        to_snake_case(list_node.name),
                        result_expr_template,
                    )
                    direct_optional_list_args.append(arg)
                    finalize_args.append(arg)
                    continue
                if _is_terminal_override(list_node.name, matcher_overrides):
                    arg = DirectOptionalListArg(
                        parse_expr,
                        list_node.name,
                        to_snake_case(list_node.name),
                        result_expr_template,
                    )
                    direct_optional_list_args.append(arg)
                    finalize_args.append(arg)
                    continue
                if (
                    list_child is not None
                    or repeat_child is not None
                    or optional_list_child is not None
                    or required_repeat_child is not None
                ):
                    raise NotImplementedError("only one dynamic stack child is currently supported")
                optional_list_child = OptionalListStackChild(
                    parse_expr,
                    list_node.name,
                    next_slot,
                    to_snake_case(list_node.name),
                    result_expr_template,
                )
                next_slot += 1
                finalize_args.append(optional_list_child)
                continue
        if isinstance(child, RepeatNode) and isinstance(child.child, ReferenceNode):
            if len(children) == 1 and not allow_top_level_repeat:
                raise NotImplementedError("unsupported preview shape for top-level repeat")
            if _is_identifier_override(child.child.name, matcher_overrides):
                raise NotImplementedError("required identifier repeat is currently unsupported")
            if _is_terminal_override(child.child.name, matcher_overrides):
                raise NotImplementedError("required terminal override repeat is currently unsupported")
            if (
                list_child is not None
                or repeat_child is not None
                or optional_list_child is not None
                or required_repeat_child is not None
            ):
                raise NotImplementedError("only one dynamic stack child is currently supported")
            required_repeat_child = RequiredRepeatStackChild(
                parse_expr, child.child.name, next_slot, to_snake_case(child.child.name)
            )
            next_slot += 1
            finalize_args.append(required_repeat_child)
            continue
        if isinstance(child, ListMacroNode) and isinstance(child.inner, ReferenceNode):
            if _is_identifier_override(child.inner.name, matcher_overrides):
                arg = DirectListArg(parse_expr, child.inner.name, to_snake_case(child.inner.name))
                direct_list_args.append(arg)
                finalize_args.append(arg)
                continue
            if _is_terminal_override(child.inner.name, matcher_overrides):
                arg = DirectListArg(parse_expr, child.inner.name, to_snake_case(child.inner.name))
                direct_list_args.append(arg)
                finalize_args.append(arg)
                continue
            if (
                list_child is not None
                or repeat_child is not None
                or optional_list_child is not None
                or required_repeat_child is not None
            ):
                raise NotImplementedError("only one dynamic stack child is currently supported")
            list_child = ListStackChild(parse_expr, child.inner.name, next_slot, to_snake_case(child.inner.name))
            next_slot += 1
            finalize_args.append(list_child)
            continue
        raise NotImplementedError(f"unsupported semantic child shape: {type(child).__name__}")
    seen_arg_names = {}
    for arg in finalize_args:
        if not hasattr(arg, "var_name"):
            continue
        var_name = arg.var_name
        count = seen_arg_names.get(var_name, 0)
        seen_arg_names[var_name] = count + 1
        if count > 0:
            arg.var_name = f"{var_name}_{count}"

    return SequenceRulePlan(
        direct_args,
        direct_optional_args,
        direct_optional_presence_args,
        direct_repeat_args,
        direct_list_args,
        direct_optional_list_args,
        stack_children,
        optional_stack_children,
        repeat_child,
        optional_list_child,
        required_repeat_child,
        list_child,
        trailing_optional_stack_child,
        finalize_args,
    )


# ---------------------------------------------------------------------------
# Sequence-element classification
# ---------------------------------------------------------------------------


@dataclass
class SeqElement:
    """One classified position in a sequence rule."""

    skip: bool
    var_name: str = ""
    cpp_type: str = ""
    by_value: bool = False
    extraction_lines: List[str] = field(default_factory=list)


def is_by_value(rule_name, rule_types):
    info = rule_types.get(rule_name)
    if info is None:
        return False
    return info.by_value or "unique_ptr<" in info.cpp_type


def _classify_literal():
    return SeqElement(skip=True)


class ExtractionKind(Enum):
    SKIP = auto()
    REFERENCE = auto()
    PARENS = auto()
    OPTIONAL = auto()
    LIST = auto()
    REPEAT = auto()
    SEQUENCE = auto()
    CHOICE = auto()


@dataclass
class ExtractionPlan:
    """Recursive description of how to extract one semantic value from a ParseResult."""

    kind: ExtractionKind
    cpp_type: str = ""
    var_name: str = ""
    by_value: bool = False
    default_initializer: str = ""
    child: "ExtractionPlan | None" = None
    child_index: int = 0
    identifier: bool = False


def plan_extraction(
    node,
    rule_types,
    excluded_rules,
    identifier_override_rules,
    optional_semantic_values=False,
):
    """Build a recursive extraction plan. Nested sequences must collapse to one semantic value."""
    if isinstance(node, LiteralNode):
        return ExtractionPlan(kind=ExtractionKind.SKIP)
    if isinstance(node, ReferenceNode):
        if node.name in identifier_override_rules:
            return ExtractionPlan(
                kind=ExtractionKind.REFERENCE,
                cpp_type="Identifier",
                var_name=to_snake_case(node.name),
                identifier=True,
            )
        if node.name in rule_types:
            return ExtractionPlan(
                kind=ExtractionKind.REFERENCE,
                cpp_type=rule_types[node.name].cpp_type,
                var_name=to_snake_case(node.name),
                by_value=is_by_value(node.name, rule_types),
                default_initializer=rule_types[node.name].default_initializer,
            )
        if node.name in excluded_rules:
            return ExtractionPlan(kind=ExtractionKind.SKIP)
        return None
    if isinstance(node, ParensNode):
        child = plan_extraction(
            node.inner,
            rule_types,
            excluded_rules,
            identifier_override_rules,
            optional_semantic_values,
        )
        return _wrap_extraction(ExtractionKind.PARENS, child)
    if isinstance(node, OptionalNode):
        child = plan_extraction(
            node.child,
            rule_types,
            excluded_rules,
            identifier_override_rules,
            optional_semantic_values,
        )
        if not optional_semantic_values:
            return _wrap_extraction(ExtractionKind.OPTIONAL, child)
        if child is None:
            return None
        if child.kind == ExtractionKind.SKIP:
            return ExtractionPlan(kind=ExtractionKind.OPTIONAL, cpp_type="bool", var_name="has_result")
        return ExtractionPlan(
            kind=ExtractionKind.OPTIONAL,
            cpp_type=f"optional<{child.cpp_type}>",
            var_name=child.var_name,
            by_value=child.by_value,
            child=child,
        )
    if isinstance(node, (ListMacroNode, RepeatNode)):
        child_node = node.inner if isinstance(node, ListMacroNode) else node.child
        child = plan_extraction(
            child_node,
            rule_types,
            excluded_rules,
            identifier_override_rules,
            optional_semantic_values,
        )
        if child is None or child.kind == ExtractionKind.SKIP:
            return None
        return ExtractionPlan(
            kind=(ExtractionKind.LIST if isinstance(node, ListMacroNode) else ExtractionKind.REPEAT),
            cpp_type=f"vector<{child.cpp_type}>",
            var_name=child.var_name,
            by_value=child.by_value,
            child=child,
        )
    if isinstance(node, SequenceNode):
        semantic = []
        for idx, sequence_child in enumerate(node.children):
            child = plan_extraction(
                sequence_child,
                rule_types,
                excluded_rules,
                identifier_override_rules,
                optional_semantic_values,
            )
            if child is None:
                return None
            if child.kind != ExtractionKind.SKIP:
                semantic.append((idx, child))
        if not semantic:
            return ExtractionPlan(kind=ExtractionKind.SKIP)
        if len(semantic) != 1:
            return None
        child_index, child = semantic[0]
        return ExtractionPlan(
            kind=ExtractionKind.SEQUENCE,
            child=child,
            child_index=child_index,
            **_plan_value_args(child),
        )
    if isinstance(node, ChoiceNode):
        alternatives = [
            plan_extraction(
                alternative,
                rule_types,
                excluded_rules,
                identifier_override_rules,
                optional_semantic_values,
            )
            for alternative in node.alternatives
        ]
        if all(alternative is not None and alternative.kind == ExtractionKind.SKIP for alternative in alternatives):
            return ExtractionPlan(kind=ExtractionKind.SKIP)
        if any(alternative is None or alternative.kind != ExtractionKind.REFERENCE for alternative in alternatives):
            return None
        first = alternatives[0]
        if any(
            alternative.cpp_type != first.cpp_type or alternative.identifier != first.identifier
            for alternative in alternatives[1:]
        ):
            return None
        return ExtractionPlan(kind=ExtractionKind.CHOICE, child=first, **_plan_value_args(first))
    return None


def _plan_value_args(plan):
    return {
        "cpp_type": plan.cpp_type,
        "var_name": plan.var_name,
        "by_value": plan.by_value,
        "default_initializer": plan.default_initializer,
    }


def _wrap_extraction(kind, child):
    if child is None or child.kind == ExtractionKind.SKIP:
        return child
    return ExtractionPlan(kind=kind, child=child, **_plan_value_args(child))


def _temp_name(target_name, suffix, depth):
    depth_suffix = f"_{depth}" if depth else ""
    return f"{target_name}_{suffix}{depth_suffix}"


def _optional_child_temp_name(target_name, depth):
    return _temp_name(target_name, "value", depth)


def emit_extraction(plan, source_expr, target_name=None, indent="\t", declare=True, depth=0):
    """Emit recursive extraction code for a plan rooted at source_expr."""
    target_name = target_name or plan.var_name
    if plan.kind == ExtractionKind.SKIP:
        return []
    if plan.kind == ExtractionKind.REFERENCE:
        if plan.identifier:
            value_expr = f"{source_expr}.Cast<IdentifierParseResult>().identifier"
        else:
            value_expr = f"transformer.Transform<{plan.cpp_type}>({source_expr})"
        prefix = "auto " if declare else ""
        return [f"{indent}{prefix}{target_name} = {value_expr};"]
    if plan.kind == ExtractionKind.PARENS:
        return emit_extraction(
            plan.child,
            f"ExtractResultFromParens({source_expr})",
            target_name,
            indent,
            declare,
            depth,
        )
    if plan.kind == ExtractionKind.SEQUENCE:
        list_expr = f"{source_expr}.Cast<ListParseResult>()"
        return emit_extraction(
            plan.child,
            f"{list_expr}.GetChild({plan.child_index})",
            target_name,
            indent,
            declare,
            depth,
        )
    if plan.kind == ExtractionKind.CHOICE:
        choice_expr = f"{source_expr}.Cast<ChoiceParseResult>()"
        return emit_extraction(
            plan.child,
            f"{choice_expr}.GetResult()",
            target_name,
            indent,
            declare,
            depth,
        )
    if plan.kind == ExtractionKind.OPTIONAL:
        opt_name = _temp_name(target_name, "opt", depth)
        lines = []
        if declare:
            lines.append(f"{indent}{plan.cpp_type} {target_name} {{}};")
        lines.append(f"{indent}auto &{opt_name} = {source_expr}.Cast<OptionalParseResult>();")
        if plan.child is None:
            lines.append(f"{indent}{target_name} = {opt_name}.HasResult();")
            return lines
        value_name = _optional_child_temp_name(target_name, depth)
        lines.append(f"{indent}if ({opt_name}.HasResult()) {{")
        lines.extend(
            emit_extraction(
                plan.child,
                f"{opt_name}.GetResult()",
                value_name,
                indent + "\t",
                declare=True,
                depth=depth + 1,
            )
        )
        value_expr = f"std::move({value_name})" if plan.child.by_value else value_name
        lines.append(f"{indent}\t{target_name} = {value_expr};")
        lines.append(f"{indent}}}")
        return lines
    if plan.kind in (ExtractionKind.LIST, ExtractionKind.REPEAT):
        item_name = _temp_name(target_name, "item", depth)
        value_name = _temp_name(target_name, "value", depth)
        lines = [f"{indent}{plan.cpp_type} {target_name};"] if declare else []
        if plan.kind == ExtractionKind.LIST:
            items_name = _temp_name(target_name, "items", depth)
            lines.append(f"{indent}auto {items_name} = ExtractParseResultsFromList({source_expr});")
            children_expr = items_name
        else:
            repeat_name = _temp_name(target_name, "repeat", depth)
            lines.append(f"{indent}auto &{repeat_name} = {source_expr}.Cast<RepeatParseResult>();")
            children_expr = f"{repeat_name}.GetChildren()"
        lines.append(f"{indent}for (auto &{item_name} : {children_expr}) {{")
        lines.extend(
            emit_extraction(
                plan.child,
                f"{item_name}.get()",
                value_name,
                indent + "\t",
                depth=depth + 1,
            )
        )
        value_expr = f"std::move({value_name})" if plan.child.by_value else value_name
        lines.append(f"{indent}\t{target_name}.push_back({value_expr});")
        lines.append(f"{indent}}}")
        return lines
    raise ValueError(f"Unsupported extraction plan kind: {plan.kind}")


def classify_sequence_element(
    child,
    idx,
    rule_types,
    excluded_rules,
    identifier_override_rules,
    optional_semantic_values=False,
):
    plan = plan_extraction(
        child,
        rule_types,
        excluded_rules,
        identifier_override_rules,
        optional_semantic_values,
    )
    if plan is None:
        return None
    if plan.kind == ExtractionKind.SKIP:
        return _classify_literal()
    return SeqElement(
        skip=False,
        var_name=plan.var_name,
        cpp_type=plan.cpp_type,
        by_value=plan.by_value,
        extraction_lines=emit_extraction(plan, f"list_pr.GetChild({idx})"),
    )


def classify_sequence_elements(
    children,
    rule_types,
    excluded_rules,
    identifier_override_rules,
    optional_semantic_values=False,
):
    elements = []
    seen = {}
    for idx, child in enumerate(children):
        elem = classify_sequence_element(
            child,
            idx,
            rule_types,
            excluded_rules,
            identifier_override_rules,
            optional_semantic_values,
        )
        if elem is None:
            return None
        if not elem.skip:
            count = seen.get(elem.var_name, 0)
            seen[elem.var_name] = count + 1
            if count > 0:
                old_name = elem.var_name
                new_name = f"{old_name}_{count}"
                elem.extraction_lines = [line.replace(old_name, new_name) for line in elem.extraction_lines]
                if old_name == "identifier":
                    elem.extraction_lines = [
                        line.replace(f".{new_name}", ".identifier") for line in elem.extraction_lines
                    ]
                elem.var_name = new_name
        elements.append(elem)
    return elements


def sequence_skip_reason(
    children,
    rule_types,
    excluded_rules,
    identifier_override_rules,
    optional_semantic_values=False,
):
    for idx, child in enumerate(children):
        if (
            classify_sequence_element(
                child,
                idx,
                rule_types,
                excluded_rules,
                identifier_override_rules,
                optional_semantic_values,
            )
            is not None
        ):
            continue
        inner = child.child if isinstance(child, OptionalNode) else child
        if isinstance(inner, ReferenceNode):
            name = inner.name
            if name not in rule_types and name not in excluded_rules and name not in identifier_override_rules:
                return f"child rule '{name}' is missing from grammar_types.yml and excluded_rules"
        return f"cannot classify element {idx} ({type(child).__name__})"
    return "unknown reason"
