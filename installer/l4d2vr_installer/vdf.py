"""Reader/writer for Valve's text KeyValues ("VDF") format.

Steam files such as ``libraryfolders.vdf``, ``loginusers.vdf``, ``config.vdf``
and ``localconfig.vdf`` use this format. The parser keeps key order,
duplicate keys and key letter case, so a parse/dump round trip of a file that
Steam wrote reproduces it byte for byte (apart from comments).
"""

from __future__ import annotations

from typing import Iterator, List, Optional, Union

Value = Union[str, "VdfNode"]

_ESCAPES = {"n": "\n", "t": "\t", "\\": "\\", '"': '"'}


class VdfError(ValueError):
    pass


class VdfNode:
    """An ordered list of ``(key, value)`` pairs; values are str or VdfNode."""

    def __init__(self, pairs: Optional[List[list]] = None):
        # Each entry is [key, value, condition-or-None].
        self.pairs: List[list] = pairs if pairs is not None else []

    # -- lookup ---------------------------------------------------------
    def _find(self, key: str) -> Optional[list]:
        lowered = key.lower()
        for pair in self.pairs:
            if pair[0].lower() == lowered:
                return pair
        return None

    def get(self, key: str, default=None):
        pair = self._find(key)
        return pair[1] if pair is not None else default

    def get_node(self, key: str) -> Optional["VdfNode"]:
        value = self.get(key)
        return value if isinstance(value, VdfNode) else None

    def get_path(self, *keys: str):
        node: Value = self
        for key in keys:
            if not isinstance(node, VdfNode):
                return None
            node = node.get(key)
            if node is None:
                return None
        return node

    def __contains__(self, key: str) -> bool:
        return self._find(key) is not None

    def __iter__(self) -> Iterator[tuple]:
        for pair in self.pairs:
            yield pair[0], pair[1]

    def items(self):
        return list(iter(self))

    def __len__(self) -> int:
        return len(self.pairs)

    def __eq__(self, other) -> bool:
        return isinstance(other, VdfNode) and self.pairs == other.pairs

    def __repr__(self) -> str:
        return f"VdfNode({self.pairs!r})"

    # -- mutation -------------------------------------------------------
    def set(self, key: str, value: Value) -> None:
        pair = self._find(key)
        if pair is None:
            self.pairs.append([key, value, None])
        else:
            pair[1] = value

    def remove(self, key: str) -> bool:
        pair = self._find(key)
        if pair is None:
            return False
        self.pairs.remove(pair)
        return True

    def ensure_node(self, key: str) -> "VdfNode":
        value = self.get(key)
        if isinstance(value, VdfNode):
            return value
        if value is not None:
            raise VdfError(f"Key {key!r} holds a string, expected a section")
        node = VdfNode()
        self.pairs.append([key, node, None])
        return node

    def to_dict(self) -> dict:
        out = {}
        for key, value in self:
            out[key] = value.to_dict() if isinstance(value, VdfNode) else value
        return out


# ---------------------------------------------------------------------------
# Parsing
# ---------------------------------------------------------------------------

def _tokens(text: str):
    """Yield (kind, value, line) where kind is 'str', '{', '}' or 'cond'."""
    i, n, line = 0, len(text), 1
    while i < n:
        c = text[i]
        if c == "\n":
            line += 1
            i += 1
        elif c in " \t\r\ufeff":
            i += 1
        elif c == "/" and text.startswith("//", i):
            while i < n and text[i] != "\n":
                i += 1
        elif c in "{}":
            yield c, c, line
            i += 1
        elif c == '"':
            i += 1
            out = []
            while True:
                if i >= n:
                    raise VdfError(f"Unterminated string starting on line {line}")
                c = text[i]
                if c == "\\" and i + 1 < n and text[i + 1] in _ESCAPES:
                    out.append(_ESCAPES[text[i + 1]])
                    i += 2
                    continue
                if c == '"':
                    i += 1
                    break
                if c == "\n":
                    line += 1
                out.append(c)
                i += 1
            yield "str", "".join(out), line
        elif c == "[":
            end = text.find("]", i)
            if end < 0:
                raise VdfError(f"Unterminated condition on line {line}")
            yield "cond", text[i:end + 1], line
            i = end + 1
        else:
            start = i
            while i < n and text[i] not in ' \t\r\n{}"':
                i += 1
            yield "str", text[start:i], line


def loads(text: str) -> VdfNode:
    root = VdfNode()
    stack = [root]
    pending_key: Optional[str] = None
    last_pair: Optional[list] = None
    for kind, value, line in _tokens(text):
        if kind == "cond":
            if last_pair is not None and pending_key is None:
                last_pair[2] = value
            continue
        if kind == "str":
            if pending_key is None:
                pending_key = value
            else:
                last_pair = [pending_key, value, None]
                stack[-1].pairs.append(last_pair)
                pending_key = None
        elif kind == "{":
            if pending_key is None:
                raise VdfError(f"Section without a name on line {line}")
            node = VdfNode()
            last_pair = [pending_key, node, None]
            stack[-1].pairs.append(last_pair)
            stack.append(node)
            pending_key = None
        elif kind == "}":
            if pending_key is not None or len(stack) == 1:
                raise VdfError(f"Unexpected '}}' on line {line}")
            stack.pop()
            last_pair = None
    if pending_key is not None:
        raise VdfError(f"Key {pending_key!r} has no value")
    if len(stack) != 1:
        raise VdfError("Unbalanced braces: missing '}'")
    return root


def load(path) -> VdfNode:
    with open(path, "r", encoding="utf-8", errors="surrogateescape") as handle:
        return loads(handle.read())


# ---------------------------------------------------------------------------
# Writing
# ---------------------------------------------------------------------------

def _quote(value: str) -> str:
    escaped = (
        value.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n").replace("\t", "\\t")
    )
    return f'"{escaped}"'


def dumps(node: VdfNode, newline: str = "\n") -> str:
    lines: List[str] = []

    def emit(current: VdfNode, depth: int) -> None:
        indent = "\t" * depth
        for key, value, condition in current.pairs:
            suffix = f" {condition}" if condition else ""
            if isinstance(value, VdfNode):
                lines.append(f"{indent}{_quote(key)}{suffix}")
                lines.append(f"{indent}{{")
                emit(value, depth + 1)
                lines.append(f"{indent}}}")
            else:
                lines.append(f"{indent}{_quote(key)}\t\t{_quote(value)}{suffix}")

    emit(node, 0)
    return newline.join(lines) + newline


def detect_newline(text: str) -> str:
    return "\r\n" if "\r\n" in text else "\n"
