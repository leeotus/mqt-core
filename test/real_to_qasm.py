#!/usr/bin/env python3
"""Convert a RevLib .real file to OpenQASM 2 for the ordering experiments.

The converter expands the .define macros that the MCT and MCT+P libraries use.
A primitive gate "t a" is a NOT, "t a b" is a controlled NOT with control a and
target b, and "t a b c" is a Toffoli with controls a and b and target c. Gates
with more than two controls are not representable in OpenQASM 2, so circuits
that need them are skipped.
"""

from __future__ import annotations

import sys
import re
from pathlib import Path


class Unsupported(Exception):
    """Raised when a circuit needs a gate that OpenQASM 2 cannot express."""


def parse_macros(
    text: str, name: str
) -> tuple[dict[str, tuple[list[str], list[list[str]]]], list[list[str]]]:
    macros: dict[str, tuple[list[str], list[list[str]]]] = {}
    body: list[list[str]] = []
    current_name: str | None = None
    current_params: list[str] = []
    current_body: list[list[str]] = []
    in_begin = False

    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith(".define"):
            parts = line.split()
            current_name = parts[1]
            current_params = parts[2:]
            current_body = []
        elif line.startswith(".enddefine"):
            if current_name is None:
                raise Unsupported(f"{name}: unbalanced .enddefine")
            macros[current_name] = (current_params, current_body)
            current_name = None
        elif line.startswith(".begin"):
            in_begin = True
        elif line.startswith(".end"):
            if in_begin:
                break
        elif current_name is not None:
            if not line.startswith("."):
                current_body.append(line.split())
        elif in_begin:
            if not line.startswith("."):
                body.append(line.split())
    return macros, body


def expand(
    tokens: list[str],
    macros: dict[str, tuple[list[str], list[list[str]]]],
    environment: dict[str, str],
    depth: int = 0,
) -> list[list[str]]:
    if depth > 50:
        raise Unsupported("macro recursion too deep")
    if re.fullmatch(r"t\d*", tokens[0]):
        return [[environment.get(operand, operand) for operand in tokens[1:]]]
    if tokens[0] not in macros:
        raise Unsupported(f"unknown gate {tokens[0]}")
    params, body = macros[tokens[0]]
    if len(params) != len(tokens) - 1:
        raise Unsupported(f"macro {tokens[0]} called with wrong arity")
    local = dict(environment)
    for parameter, argument in zip(params, tokens[1:]):
        local[parameter] = environment.get(argument, argument)
    result: list[list[str]] = []
    for statement in body:
        result.extend(expand(statement, macros, local, depth + 1))
    return result


def convert(path: Path) -> str:
    text = path.read_text(errors="replace")
    macros, body = parse_macros(text, str(path))

    num_vars = 0
    variables: list[str] = []
    inputs: list[str] = []
    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith(".numvars"):
            num_vars = int(stripped.split()[1])
        elif stripped.startswith(".variables"):
            variables = stripped.split()[1:]
        elif stripped.startswith(".inputs"):
            inputs = stripped.split()[1:]
        elif stripped.startswith(".begin"):
            break

    if len(variables) != num_vars or len(inputs) != num_vars:
        raise Unsupported("variable or input count mismatch")
    index = {variable: i for i, variable in enumerate(variables)}

    statements: list[list[str]] = []
    for statement in body:
        statements.extend(expand(statement, macros, {}))

    qasm = ["OPENQASM 2.0;", 'include "qelib1.inc";', f"qreg q[{num_vars}];"]
    for variable, value in zip(variables, inputs):
        if value == "1":
            qasm.append(f"x q[{index[variable]}];")

    for operands in statements:
        qubits = [index[operand] for operand in operands]
        if len(qubits) == 1:
            qasm.append(f"x q[{qubits[0]}];")
        elif len(qubits) == 2:
            qasm.append(f"cx q[{qubits[0]}],q[{qubits[1]}];")
        elif len(qubits) == 3:
            qasm.append(f"ccx q[{qubits[0]}],q[{qubits[1]}],q[{qubits[2]}];")
        else:
            raise Unsupported(
                f"gate with {len(qubits)} qubits is not expressible"
            )
    return "\n".join(qasm) + "\n"


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: real_to_qasm.py <input-dir> <output-dir>")
        return 2
    source = Path(sys.argv[1])
    target = Path(sys.argv[2])
    target.mkdir(parents=True, exist_ok=True)
    converted = 0
    skipped = 0
    for real in sorted(source.rglob("*.real")):
        try:
            qasm = convert(real)
        except Exception as error:  # noqa: BLE001
            print(f"skip {real.name}: {error}")
            skipped += 1
            continue
        (target / (real.stem + ".qasm")).write_text(qasm)
        converted += 1
    print(f"converted {converted} circuits, skipped {skipped}, into {target}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
