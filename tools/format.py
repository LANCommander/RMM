#!/usr/bin/env python3
"""Format RMM's C/C++ sources: clang-format (.clang-format) plus C#-style
vertical spacing rules that clang-format cannot express:

  * a blank line before `return` and before block statements (if, for, while,
    do, switch, try), unless it is the first statement in a block, an
    `else if`, a do-while's trailing `while (...);`, or the body of a braceless
    if/else/loop/case;
  * a blank line after a block's closing `}` (StyleCop SA1513), or after the
    last line of a braceless if/else/loop body, unless the next
    line is another `}`, else/catch/while, a `)`/`,`/`;` continuation, or a
    preprocessor directive.

Usage: python tools/format.py [--check] [paths...]   (default: whole repo)
"""

import argparse
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE_DIRS = ["src", "include", "plugins", "tests", "tools"]
EXTENSIONS = (".c", ".cc", ".cpp", ".cxx", ".h", ".hpp")
CLANG_FORMAT_FALLBACKS = [
    r"C:\Program Files\LLVM\bin\clang-format.exe",
    r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\Llvm\x64\bin\clang-format.exe",
    r"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\x64\bin\clang-format.exe",
]

SPACED_STATEMENT_RE = re.compile(r"^(return|if|for|while|do|switch|try)\b")
DO_WHILE_TAIL_RE = re.compile(r"^while\b.*;$")
CONTROL_HEADER_RE = re.compile(r"^(\}\s*)?(if|for|while|else|do)\b")
CLOSE_BRACE_RE = re.compile(r"^\};?$")
NO_BLANK_AFTER_BRACE_RE = re.compile(r"^(\}|\)|,|;|#|else\b|catch\b|while\b)")


def find_clang_format():
    exe = shutil.which("clang-format")
    if exe:
        return exe
    for path in CLANG_FORMAT_FALLBACKS:
        if os.path.isfile(path):
            return path
    sys.exit("clang-format not found (install LLVM or put it on PATH)")


def collect_files(paths):
    if not paths:
        paths = [os.path.join(ROOT, d) for d in SOURCE_DIRS]
    files = []
    for path in paths:
        if os.path.isfile(path):
            files.append(path)
            continue
        for dirpath, _, names in os.walk(path):
            files.extend(os.path.join(dirpath, n) for n in names if n.endswith(EXTENSIONS))
    return sorted(files)


class LineInfo:
    __slots__ = ("code", "comment_only", "preprocessor")

    def __init__(self, code, comment_only, preprocessor):
        self.code = code  # line text with comments and literal contents removed, stripped
        self.comment_only = comment_only  # no code, but has (or is inside) a comment
        self.preprocessor = preprocessor  # directive or macro continuation line


def analyze(lines):
    """Strip comments/literals per line, tracking block comments and macro continuations."""
    infos = []
    in_block = False
    in_macro = False
    for raw in lines:
        text = raw.rstrip("\r\n")
        preprocessor = in_macro or (not in_block and text.lstrip().startswith("#"))
        in_macro = preprocessor and text.rstrip().endswith("\\")

        out = []
        had_comment = in_block
        i, n = 0, len(text)
        while i < n:
            if in_block:
                end = text.find("*/", i)
                if end < 0:
                    i = n
                else:
                    in_block = False
                    i = end + 2
                continue
            c = text[i]
            if text.startswith("//", i):
                had_comment = True
                break
            if text.startswith("/*", i):
                had_comment = True
                in_block = True
                i += 2
                continue
            if c in "\"'":
                j = i + 1
                while j < n and text[j] != c:
                    j += 2 if text[j] == "\\" else 1
                out.append(c + c)
                i = j + 1
                continue
            out.append(c)
            i += 1

        code = "".join(out).strip()
        infos.append(LineInfo(code, not code and had_comment, preprocessor))
    return infos


def indent(line):
    return len(line) - len(line.lstrip(" \t"))


def next_nonblank(lines, i):
    while i < len(lines) and not lines[i].strip():
        i += 1
    return i if i < len(lines) else -1


def ends_braceless_body(lines, infos, i, next_indent):
    """True if line i ends a braceless control-statement body and the line at
    next_indent continues the enclosing block. Walks up the chain of
    less-indented lines; the one at next_indent must be an if/else/loop header."""
    cur = indent(lines[i])
    if next_indent >= cur:
        return False
    for j in range(i - 1, -1, -1):
        info = infos[j]
        if not info.code or info.preprocessor or indent(lines[j]) >= cur:
            continue
        cur = indent(lines[j])
        if cur < next_indent:
            return False
        if cur == next_indent:
            return bool(CONTROL_HEADER_RE.match(info.code)) and not info.code.endswith(("{", ";"))
    return False


def apply_spacing(lines):
    infos = analyze(lines)
    insert_before = set()

    for i, info in enumerate(infos):
        if info.preprocessor or not info.code:
            continue

        # Blank line before return/block statements (placed above any attached comment).
        # Only after a completed statement (`;` or `}`): this excludes the first
        # statement in a block, braceless bodies, case labels and `else if`.
        if SPACED_STATEMENT_RE.match(info.code) and not DO_WHILE_TAIL_RE.match(info.code):
            j = i - 1
            while j >= 0 and infos[j].comment_only:
                j -= 1
            if j >= 0 and infos[j].code and not infos[j].preprocessor and infos[j].code[-1] in ";}":
                insert_before.add(j + 1)

        # Blank line after the end of a braceless if/else/loop body.
        if info.code.endswith(";"):
            n = next_nonblank(lines, i + 1)
            if (
                n == i + 1
                and not infos[n].preprocessor
                and (infos[n].comment_only or not NO_BLANK_AFTER_BRACE_RE.match(infos[n].code))
                and ends_braceless_body(lines, infos, i, indent(lines[n]))
            ):
                insert_before.add(n)

        # Blank line after a block's closing brace.
        if CLOSE_BRACE_RE.match(info.code) and i + 1 < len(lines):
            nxt = infos[i + 1]
            if nxt.code and not nxt.preprocessor and not NO_BLANK_AFTER_BRACE_RE.match(nxt.code):
                insert_before.add(i + 1)
            elif nxt.comment_only:
                insert_before.add(i + 1)

    if not insert_before:
        return lines
    newline = "\r\n" if lines and lines[0].endswith("\r\n") else "\n"
    out = []
    for i, line in enumerate(lines):
        if i in insert_before:
            out.append(newline)
        out.append(line)
    return out


def format_text(clang_format, path, text):
    def run_clang(src):
        res = subprocess.run(
            [clang_format, "--style=file", f"--assume-filename={path}"],
            input=src.encode("utf-8"),
            capture_output=True,
            check=True,
        )
        return res.stdout.decode("utf-8")

    text = run_clang(text)
    text = "".join(apply_spacing(text.splitlines(keepends=True)))
    return run_clang(text)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true", help="report unformatted files, change nothing")
    parser.add_argument("paths", nargs="*")
    args = parser.parse_args()

    clang_format = find_clang_format()
    dirty = []
    for path in collect_files(args.paths):
        with open(path, encoding="utf-8", newline="") as f:
            original = f.read()
        formatted = format_text(clang_format, path, original)
        if formatted != original:
            dirty.append(path)
            if not args.check:
                with open(path, "w", encoding="utf-8", newline="") as f:
                    f.write(formatted)

    for path in dirty:
        print(("needs formatting: " if args.check else "formatted: ") + os.path.relpath(path, ROOT))
    return 1 if (args.check and dirty) else 0


if __name__ == "__main__":
    sys.exit(main())
