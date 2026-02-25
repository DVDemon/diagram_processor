#!/usr/bin/env python3
"""
Convert JSON (DrawIO parser output format) to DOT graph format.

Input format: { components, requests, parent_child, success }
Output: DOT digraph suitable for Graphviz (dot, neato, etc.)

Usage:
  python json2dot.py < input.json
  python json2dot.py input.json
  python json2dot.py input.json -o output.dot
"""

import argparse
import json
import re
import sys


def escape_dot(s: str) -> str:
    """Escape string for use in DOT label/ID."""
    if not s:
        return ""
    s = s.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")
    return s


def dot_id(node_id: str) -> str:
    """Return DOT-safe node ID."""
    if re.match(r"^[a-zA-Z_][a-zA-Z0-9_]*$", node_id):
        return node_id
    return f'"{escape_dot(node_id)}"'


def json_to_dot(data: dict) -> str:
    out = ["digraph G {"]
    out.append('  rankdir=LR;')
    out.append('  node [shape=box, fontname="Helvetica"];')
    out.append("")

    components = data.get("components", [])
    requests = data.get("requests", [])
    parent_child = data.get("parent_child", [])

    comp_by_id = {c["id"]: c for c in components}

    # Nodes
    for c in components:
        node_id = dot_id(c["id"])
        name = c.get("name", c.get("id", ""))
        c4_type = c.get("c4_type", "")
        label = escape_dot(name)
        if c4_type:
            label = f"{escape_dot(name)}\\n{c4_type}"
        out.append(f'  {node_id} [label="{label}"];')
    out.append("")

    # Edges from requests
    for r in requests:
        src = r.get("component_source_id")
        tgt = r.get("component_target_id")
        if not src or not tgt:
            continue
        if src not in comp_by_id or tgt not in comp_by_id:
            continue
        desc = r.get("description", "")
        if desc:
            out.append(f'  {dot_id(src)} -> {dot_id(tgt)} [label="{escape_dot(desc)}"];')
        else:
            out.append(f"{dot_id(src)} -> {dot_id(tgt)};")
    out.append("")

    # Optional: subgraphs from parent_child (cluster by parent)
    if parent_child:
        parents = {}
        for pc in parent_child:
            pid = pc.get("parent_id")
            cid = pc.get("child_id")
            if pid and cid and pid in comp_by_id and cid in comp_by_id:
                parents.setdefault(pid, []).append(cid)

        for pid, children in parents.items():
            if pid in comp_by_id:
                parent_name = comp_by_id[pid].get("name", pid)
                out.append(f'  subgraph cluster_{pid.replace("-", "_")} {{')
                out.append(f'    label="{escape_dot(parent_name)}";')
                for cid in children:
                    out.append(f"    {dot_id(cid)};")
                out.append("  }")
                out.append("")

    out.append("}")
    return "\n".join(out)


def main():
    parser = argparse.ArgumentParser(description="Convert JSON to DOT format")
    parser.add_argument("input", nargs="?", help="Input JSON file (default: stdin)")
    parser.add_argument("-o", "--output", help="Output DOT file (default: stdout)")
    args = parser.parse_args()

    if args.input:
        with open(args.input, encoding="utf-8") as f:
            data = json.load(f)
    else:
        data = json.load(sys.stdin)

    if not data.get("success", True):
        sys.stderr.write("Warning: input has success=false\n")

    dot = json_to_dot(data)

    if args.output:
        with open(args.output, "w", encoding="utf-8") as f:
            f.write(dot)
    else:
        print(dot)


if __name__ == "__main__":
    main()
