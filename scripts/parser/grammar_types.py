import sys
from dataclasses import dataclass

try:
    import yaml
except ImportError:
    yaml = None


@dataclass
class GrammarTypeInfo:
    """Per-rule type metadata loaded from grammar_types.yml."""

    cpp_type: str
    by_value: bool = False  # True for unique_ptr<T>, vector<unique_ptr<T>> (non-copyable)
    default_initializer: str = ""  # Optional enum member/full C++ initializer for Optional(...) values
    pass_location: bool = False  # Pass the parse result's offset to the hand-written body


def load_grammar_types_yaml(types_file):
    if yaml is None:
        print("Error: PyYAML is required. Install with: pip install pyyaml", file=sys.stderr)
        sys.exit(1)

    if not types_file.is_file():
        print(f"Error: {types_file} not found.", file=sys.stderr)
        sys.exit(1)

    with types_file.open("r", encoding="utf-8") as f:
        data = yaml.safe_load(f)

    if not isinstance(data, dict):
        print(f"Error: {types_file} is malformed (expected a top-level mapping).", file=sys.stderr)
        sys.exit(1)

    return data


def validate_grammar_types(types_file, data, rule_types, excluded_rules):
    matcher_overrides = data.get("matcher_rule_overrides", {})
    if not isinstance(matcher_overrides, dict):
        print(f"Error: matcher_rule_overrides in {types_file} must be a mapping.", file=sys.stderr)
        sys.exit(1)

    errors = []
    matcher_rules = set(matcher_overrides.keys())
    excluded_overlap = sorted(matcher_rules.intersection(excluded_rules))
    if excluded_overlap:
        errors.append("Rules cannot be listed in both matcher_rule_overrides and excluded_rules:")
        errors.extend(f"  - {name}" for name in excluded_overlap)

    for name, override in matcher_overrides.items():
        if not isinstance(override, dict):
            errors.append(f"matcher_rule_overrides entry for '{name}' must be a mapping")

    type_overlap = sorted(
        name
        for name in matcher_rules.intersection(rule_types)
        if isinstance(matcher_overrides[name], dict) and not matcher_overrides[name].get("allow_type_overlap")
    )
    if type_overlap:
        errors.append("Matcher override rules with normal return types must set allow_type_overlap: true:")
        errors.extend(f"  - {name}" for name in type_overlap)

    if errors:
        print(f"Error: {types_file} contains inconsistent rule metadata:", file=sys.stderr)
        for error in errors:
            print(error, file=sys.stderr)
        sys.exit(1)


def load_grammar_types(types_file):
    """
    Loads grammar_types.yml and returns (rule_types, excluded_rules) where
    rule_types maps rule name -> GrammarTypeInfo (cpp_type + by_value + default_initializer), and excluded_rules is
    the set of rules that should be skipped during stub generation.
    Override rules default to by_value=False; a startswith('unique_ptr<') fallback covers
    any override types that are move-only.
    """
    data = load_grammar_types_yaml(types_file)

    rule_types = {}
    rule_to_source = {}  # tracks where each rule was first seen for error messages
    duplicates = []

    def register(name, cpp_type, by_value, default_initializer, pass_location, source):
        name = str(name)
        if name in rule_types:
            duplicates.append(f"  '{name}' in '{source}' (already listed in '{rule_to_source[name]}')")
        else:
            rule_types[name] = GrammarTypeInfo(
                cpp_type=str(cpp_type),
                by_value=by_value,
                default_initializer=str(default_initializer or ""),
                pass_location=pass_location,
            )
            rule_to_source[name] = source

    # Top-level overrides: RuleName -> "type" string OR {type, by_value, default_initializer} dict.
    # default_initializer usually names an enum member (e.g. "INNER"), but full C++ initializers
    # that start with "=" or "{" are also accepted by generate_transformer_trampoline.py.
    overrides = data.get("overrides", {})
    if isinstance(overrides, dict):
        for name, value in overrides.items():
            if isinstance(value, str):
                register(name, value, False, "", False, "overrides")
            elif isinstance(value, dict):
                cpp_type = value.get("type", "")
                by_value = bool(value.get("by_value", False))
                default_initializer = value.get("default_initializer", "")
                pass_location = bool(value.get("pass_location", False))
                register(name, cpp_type, by_value, default_initializer, pass_location, "overrides")

    # Category entries: CategoryName -> {type: "...", by_value: bool, default_initializer: "...", rules: [...]}
    for key, value in data.items():
        if key in (
            "overrides",
            "excluded_rules",
            "matcher_rule_overrides",
            "packrat_memoized_rules",
            "rule_labels",
            "collapsible_rules",
            "expression_depth_nesting_rules",
            "expression_depth_chain_rules",
            "additional_transform_result_types",
        ):
            continue
        if not isinstance(value, dict):
            continue
        cpp_type = value.get("type")
        rules = value.get("rules", [])
        if not cpp_type or not isinstance(rules, list):
            continue
        by_value = bool(value.get("by_value", False))
        default_initializer = value.get("default_initializer", "")
        pass_location = bool(value.get("pass_location", False))
        for name in rules:
            register(name, cpp_type, by_value, default_initializer, pass_location, key)

    if duplicates:
        print(f"Error: {types_file} contains duplicate rule listings:", file=sys.stderr)
        for msg in duplicates:
            print(msg, file=sys.stderr)
        sys.exit(1)

    excluded_rules = set(data.get("excluded_rules", []))
    validate_grammar_types(types_file, data, rule_types, excluded_rules)
    return rule_types, excluded_rules


def load_additional_transform_result_types(types_file):
    """Load result types used by transformer infrastructure rather than grammar rules."""
    data = load_grammar_types_yaml(types_file)
    result_types = data.get("additional_transform_result_types", [])
    if not isinstance(result_types, list) or not all(isinstance(result_type, str) for result_type in result_types):
        print(f"Error: additional_transform_result_types in {types_file} must be a list of strings.", file=sys.stderr)
        sys.exit(1)
    return result_types


def load_matcher_rule_overrides(types_file):
    """Load matcher_rule_overrides from grammar_types.yml."""
    data = load_grammar_types_yaml(types_file)

    overrides = data.get("matcher_rule_overrides", {})
    if not isinstance(overrides, dict):
        print(f"Error: matcher_rule_overrides in {types_file} must be a mapping.", file=sys.stderr)
        sys.exit(1)
    return overrides


def load_rule_name_list(types_file, key, known_rules=None):
    """Load a list of grammar rule names from grammar_types.yml."""
    data = load_grammar_types_yaml(types_file)

    rules = data.get(key, [])
    if not isinstance(rules, list):
        print(f"Error: {key} in {types_file} must be a list.", file=sys.stderr)
        sys.exit(1)
    rules = [str(rule) for rule in rules]

    if known_rules is not None:
        known_rules = set(known_rules)
        missing_rules = sorted(rule for rule in rules if rule not in known_rules)
        if missing_rules:
            print(f"Error: {key} in {types_file} contains unknown grammar rules:", file=sys.stderr)
            for rule in missing_rules:
                print(f"  - {rule}", file=sys.stderr)
            sys.exit(1)

    return rules


def load_rule_labels(types_file, known_rules=None):
    """Load rule_labels from grammar_types.yml as a list of (rule, label) pairs."""
    data = load_grammar_types_yaml(types_file)

    labels = data.get("rule_labels", {})
    if not isinstance(labels, dict):
        print(f"Error: rule_labels in {types_file} must be a mapping from label to a list of rules.", file=sys.stderr)
        sys.exit(1)
    result = []
    seen = {}
    for label, rules in labels.items():
        if not isinstance(rules, list):
            print(f"Error: rule_labels entry {label!r} in {types_file} must be a list of rules.", file=sys.stderr)
            sys.exit(1)
        for rule in rules:
            rule = str(rule)
            if rule in seen:
                print(
                    f"Error: rule {rule} has two labels in {types_file}: {seen[rule]!r} and {label!r}.", file=sys.stderr
                )
                sys.exit(1)
            seen[rule] = label
            result.append((rule, str(label)))

    if known_rules is not None:
        known_rules = set(known_rules)
        missing_rules = sorted(rule for rule, _ in result if rule not in known_rules)
        if missing_rules:
            print(f"Error: rule_labels in {types_file} contains unknown grammar rules:", file=sys.stderr)
            for rule in missing_rules:
                print(f"  - {rule}", file=sys.stderr)
            sys.exit(1)

    return result


def load_packrat_memoized_rules(types_file, known_rules=None):
    """Load packrat_memoized_rules from grammar_types.yml."""
    return load_rule_name_list(types_file, "packrat_memoized_rules", known_rules)


def load_expression_depth_rules(types_file, known_rules=None):
    """Load expression_depth_nesting_rules and expression_depth_chain_rules from grammar_types.yml."""
    return (
        load_rule_name_list(types_file, "expression_depth_nesting_rules", known_rules),
        load_rule_name_list(types_file, "expression_depth_chain_rules", known_rules),
    )


def load_collapsible_rules(types_file, known_rules=None):
    """Load collapsible_rules from grammar_types.yml."""
    return load_rule_name_list(types_file, "collapsible_rules", known_rules)
