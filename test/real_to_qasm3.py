#!/usr/bin/env python3
"""Convert RevLib ``.real`` files to OpenQASM 3 for the downstream experiments.

The converter expands the ``.define`` macros that the MCT and MCT+P libraries
use. A primitive gate ``t a`` is a NOT, ``t a b`` is a controlled NOT with
control ``a`` and target ``b``, and ``t a b c`` is a Toffoli with controls
``a`` and ``b`` and target ``c``. Gates with more controls become OpenQASM 3
multi-controlled ``X`` gates, which OpenQASM 2 cannot express.

The output is meant for tools that read OpenQASM 3, such as MQT DDSIM and the
MQT Core importer. The companion script ``real_to_qasm.py`` writes the subset
of the format that OpenQASM 2 tools accept.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path


class Unsupported(Exception):
    """Raised when a circuit cannot be expressed."""


def parseMacros(
    text: str, name: str
) -> tuple[dict[str, tuple[list[str], list[list[str]]]], list[list[str]]]:
    macros: dict[str, tuple[list[str], list[list[str]]]] = {}
    body: list[list[str]] = []
    currentName: str | None = None
    currentParams: list[str] = []
    currentBody: list[list[str]] = []
    inBegin = False

    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith(".define"):
            parts = line.split()
            currentName = parts[1]
            currentParams = parts[2:]
            currentBody = []
        elif line.startswith(".enddefine"):
            if currentName is None:
                raise Unsupported(f"{name}: unbalanced .enddefine")
            macros[currentName] = (currentParams, currentBody)
            currentName = None
        elif line.startswith(".begin"):
            inBegin = True
        elif line.startswith(".end"):
            if inBegin:
                break
        elif currentName is not None:
            if not line.startswith("."):
                currentBody.append(line.split())
        elif inBegin:
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
    macros, body = parseMacros(text, str(path))

    numVars = 0
    variables: list[str] = []
    inputs: list[str] = []
    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith(".numvars"):
            numVars = int(stripped.split()[1])
        elif stripped.startswith(".variables"):
            variables = stripped.split()[1:]
        elif stripped.startswith(".inputs"):
            inputs = stripped.split()[1:]
        elif stripped.startswith(".begin"):
            break
    else:
        raise Unsupported("missing .begin")

    if not variables:
        variables = [f"x{i}" for i in range(numVars)]
        numVars = len(variables)
    if not inputs:
        inputs = ["0"] * numVars
    if len(variables) != numVars or len(inputs) != numVars:
        raise Unsupported("variable or input count mismatch")

    index = {variable: i for i, variable in enumerate(variables)}
    statements: list[list[str]] = []
    for statement in body:
        statements.extend(expand(statement, macros, {}))

    qasm = [
        "OPENQASM 3.0;",
        'include "stdgates.inc";',
        f"qubit[{numVars}] q;",
    ]
    for variable, value in zip(variables, inputs):
        if value == "1":
            qasm.append(f"x q[{index[variable]}];")

    for operands in statements:
        qubits = [index[operand] for operand in operands]
        if len(qubits) == 1:
            qasm.append(f"x q[{qubits[0]}];")
        elif len(qubits) == 2:
            qasm.append(f"cx q[{qubits[0]}], q[{qubits[1]}];")
        else:
            controls = ", ".join(f"q[{q}]" for q in qubits[:-1])
            qasm.append(f"ctrl({len(qubits) - 1}) @ x {controls}, q[{qubits[-1]}];")
    return "\n".join(qasm) + "\n"


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: real_to_qasm3.py <input-dir> <output-dir>")
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
