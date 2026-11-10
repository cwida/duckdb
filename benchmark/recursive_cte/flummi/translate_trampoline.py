#!/usr/bin/env python3
"""Mechanical trampoline translation for Flummi-emitted label-dispatch recursive CTEs.

Input shape (machine-emitted):
    WITH RECURSIVE
      "<rel>"(<cols>) AS (
        (<seed with constant label>)
          UNION ALL
        (WITH <dataflow blocks, entries filter the label column> <exit union>)
      )
    <outer query over label IS NULL rows>;

Translation: one BRANCH per label, each carrying the member body verbatim except
that entry filters of OTHER labels become WHERE FALSE (the optimizer's empty-result
propagation then specializes each branch plan). A selector column derived from the
label column routes rows: NULL label emits (0), label 'start.k' routes to branch k.
Faithfulness rests on entries being the only reads of the recursive relation, which
is checked, and on the per-iteration frontier being single-label, which holds for
sequential source programs and is enforced end-to-end by byte-identical results.
With a whole-frontier barrier, labels wait across rounds, so every branch gets
MERGE (one bag wave per round) to process per-label populations wholesale like a
recursive CTE iteration. Absence of a barrier does not by itself imply a
single-label frontier; leaving MERGE off rests on the sequential-program
invariant above and the byte-identical check.
"""

import os
import re
import sys


def match_paren(text, open_idx):
    assert text[open_idx] == "("
    depth = 0
    in_string = False
    i = open_idx
    while i < len(text):
        c = text[i]
        if in_string:
            if c == "'":
                in_string = False
        elif c == "'":
            in_string = True
        elif c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    raise ValueError("unbalanced parentheses")


def main(path_in, path_out):
    sql = open(path_in).read()

    header = re.search(r'WITH RECURSIVE\s*\n\s*"🔄"\(([^)]*)\) AS \(', sql)
    assert header, "unexpected frame"
    cols = header.group(1)
    # header.end() points just past the 'AS (' opening paren; find its match
    def_open = sql.rindex("(", 0, header.end())
    def_close = match_paren(sql, def_open)
    body = sql[def_open + 1 : def_close]
    outer = sql[def_close + 1 :].lstrip("\n")

    seed_open = body.index("(")
    seed_close = match_paren(body, seed_open)
    seed = body[seed_open + 1 : seed_close]
    rest = body[seed_close + 1 :]
    union_match = re.search(r"\s*UNION ALL\s*", rest)
    assert union_match and union_match.start() == 0, "seed must be followed by UNION ALL"
    member_open = rest.index("(", union_match.end() - 1)
    member_close = match_paren(rest, member_open)
    member = rest[member_open + 1 : member_close]
    assert rest[member_close + 1 :].strip() == "", "unexpected trailing member content"

    # whole-frontier barriers: NOT EXISTS over the previous iteration's full table
    # becomes a read of the frozen round snapshot, with the label as selector value
    barrier_pattern = re.compile(
        r"NOT EXISTS \(\(SELECT NULL\s*\n\s*FROM   \"🔄\"\s*\n\s*" r"WHERE  \"🔄\".\"🏷️\" <> '(start\.(\d+))'\)\)"
    )

    def rewrite_barrier(m):
        return (
            "NOT EXISTS ((SELECT NULL\n"
            '                             FROM   frontier."🔄" AS "🛸"\n'
            '                             WHERE  "🛸"."b" <> %s))' % m.group(2)
        )

    member, barrier_count = barrier_pattern.subn(rewrite_barrier, member)

    labels = sorted(
        set(re.findall(r"IS NOT DISTINCT FROM '(start\.\d+)'", member)),
        key=lambda name: int(name.split(".")[1]),
    )
    assert labels and labels == ["start.%d" % (i + 1) for i in range(len(labels))], labels
    # the recursive relation may only be read by the label-filtered entries
    entry_pattern = r'FROM   "🔄"\n\s*WHERE  "🔄"."🏷️" IS NOT DISTINCT FROM \'(start\.\d+)\''
    entries = re.findall(entry_pattern, member)
    assert sorted(entries) == sorted(labels), (sorted(entries), labels)
    assert member.count('FROM   "🔄"') == len(labels), "recursive relation read outside the entries"
    seed_label = re.search(r"'(start\.\d+)'", seed).group(1)

    selector_case = (
        'CASE WHEN q."🏷️" IS NULL THEN 0 '
        + " ".join("WHEN q.\"🏷️\" = '%s' THEN %d" % (label, i + 1) for i, label in enumerate(labels))
        + " END"
    )

    def parse_blocks(text):
        """Splits a member of shape WITH "a"(..) AS (..), "b"(..) AS (..) <exit>."""
        assert text.lstrip().startswith("WITH"), text[:40]
        pos = text.index("WITH") + 4
        blocks = []  # (name, header_text, body)
        while True:
            m = re.compile(r'\s*"([^"]+)"\([^)]*\) AS \(').match(text, pos)
            if not m:
                break
            body_open = m.end() - 1
            body_close = match_paren(text, body_open)
            blocks.append(
                (
                    m.group(1),
                    text[m.start() : body_open + 1],
                    text[body_open + 1 : body_close],
                )
            )
            pos = body_close + 1
            comma = re.compile(r"\s*,").match(text, pos)
            if not comma:
                break
            pos = comma.end()
        return blocks, text[pos:]

    def slice_member(branch_member, dead_entries):
        """Static emptiness propagation: entries of other labels are empty, a block is
        empty when any referenced block is (inner joins only, the one aggregate carries
        HAVING COUNT(*) > 0), empty exit arms drop, unreferenced blocks drop."""
        assert not re.search(r"LEFT|RIGHT|FULL|OUTER", branch_member), "outer joins unsupported"
        blocks, exit_text = parse_blocks(branch_member)
        names = [name for name, _, _ in blocks]
        name_set = set(names)
        empty = set(dead_entries)
        ref = re.compile(r'"([^"]+)"')

        def split_arms(text):
            arms = []
            arm_pos = text.index("(")
            while True:
                arm_close = match_paren(text, arm_pos)
                arms.append(text[arm_pos : arm_close + 1])
                m = re.compile(r"\s*UNION ALL\s*\(").match(text, arm_close + 1)
                if not m:
                    assert text[arm_close + 1 :].strip() == "", text[arm_close + 1 : arm_close + 60]
                    break
                arm_pos = m.end() - 1
            return arms

        def arm_is_empty(arm):
            return bool({d for d in ref.findall(arm) if d in name_set} & empty)

        deps = {}
        rebuilt = []  # (name, final_text or None when empty)
        for name, header, body in blocks:
            if name in empty:
                rebuilt.append((name, None))
                deps[name] = set()
                continue
            if "UNION ALL" in body:
                # control-flow joins: empty only when every arm is, and dead arms
                # must drop so no reference to a removed block survives
                arms = [arm for arm in split_arms(body) if not arm_is_empty(arm)]
                if not arms:
                    empty.add(name)
                    rebuilt.append((name, None))
                    deps[name] = set()
                    continue
                body = "\n         " + "\n           UNION ALL\n         ".join(arms) + "\n       "
                rebuilt.append((name, header + body + ")"))
            else:
                if {d for d in ref.findall(body) if d in name_set and d != name} & empty:
                    empty.add(name)
                    rebuilt.append((name, None))
                    deps[name] = set()
                    continue
                rebuilt.append((name, header + body + ")"))
            deps[name] = {d for d in ref.findall(body) if d in name_set and d != name}
        kept_arms = [arm for arm in split_arms(exit_text) if not arm_is_empty(arm)]
        assert kept_arms, "a branch must keep at least its own exits"
        needed = set()
        work = [d for arm in kept_arms for d in ref.findall(arm) if d in name_set]
        while work:
            d = work.pop()
            if d in needed:
                continue
            needed.add(d)
            work.extend(deps[d])
        assert not (needed & empty), sorted(needed & empty)[:3]
        kept_blocks = [text for name, text in rebuilt if name in needed and text is not None]
        assert len(kept_blocks) == len(needed)
        assert len(kept_blocks) < len(blocks), "slicing must remove something"
        return "WITH" + ",".join(kept_blocks) + "\n" + "\n       UNION ALL\n     ".join(kept_arms)

    branches = []
    for branch_idx, branch_label in enumerate(labels, start=1):
        branch_member = member
        dead_entries = []
        for other in labels:
            if other == branch_label:
                continue
            old = 'WHERE  "🔄"."🏷️" IS NOT DISTINCT FROM \'%s\'' % other
            assert old in branch_member
            branch_member = branch_member.replace(old, "WHERE  FALSE")
            dead_entries.append(other)
        branch_member = slice_member(branch_member, dead_entries)
        # with whole-frontier barriers, labels wait across rounds and per-label
        # populations must be processed wholesale like a recursive CTE iteration
        modifier = " MERGE" if barrier_count else ""
        branches.append(
            '  BRANCH %d%s: (SELECT %s AS "b", q.* FROM (%s\n) AS q)'
            % (branch_idx, modifier, selector_case, branch_member)
        )

    out = []
    out.append("-- Trampoline formulation, mechanically translated from %s" % os.path.basename(path_in))
    out.append("-- by flummi/translate_trampoline.py: one branch per label, entry filters of")
    if barrier_count:
        out.append("-- other labels become FALSE, the selector mirrors the label, and the SYNC")
        out.append("-- barrier becomes a frontier read with MERGE branches.")
    else:
        out.append("-- other labels become FALSE, the selector mirrors the label.")
    out.append("WITH RECURSIVE")
    out.append('  "🔄"("b", %s) USING TRAMPOLINE ("b") AS (' % cols)
    out.append('  (SELECT %s AS "b", q.* FROM (%s\n) AS q)' % (selector_case, seed))
    out.extend(branches)
    out.append("  )")
    out.append(outer.rstrip())
    result = "\n".join(out) + "\n"
    assert seed_label in result
    open(path_out, "w").write(result)
    print("wrote %s: %d labels, %d barriers, %d bytes" % (path_out, len(labels), barrier_count, len(result)))


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
