#!/usr/bin/env python3
# Copyright (c) 2026 Kamil Kiełbasa
# SPDX-License-Identifier: MIT
"""Rules the C sources follow, checked over library/, include/, tests/ and
samples/:

- no test is skipped with ztest_test_skip();
- no file reaches 800 lines;
- no function result is cast to (void);
- functions are defined in the order they are declared: static ones in the
  order of the "Static function declarations" section, the others in the
  order of the header that declares them.

Prints every violation as file:line and exits with 1 if there is any.
"""

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
DIRS = ('library', 'include', 'tests', 'samples')
LINE_LIMIT = 800

VOID_CAST = re.compile(r'\(void\)\s*[A-Za-z_]\w*\s*\(')
BANNER = re.compile(r'^/\* ([A-Z][^-]*?) -{3,}')
NAME = re.compile(r'([A-Za-z_]\w*)\s*\(')

STATIC_DECLARATIONS = 'Static function declarations'
STATIC_DEFINITIONS = 'Static function definitions'
INTERFACE_DECLARATIONS = ('Module interface function declarations',
                          'Function declarations')
INTERFACE_DEFINITIONS = 'Module interface function definitions'


def sources():
    for top in DIRS:
        for path in sorted((ROOT / top).rglob('*.[ch]')):
            parts = path.relative_to(ROOT).parts
            if any(p.startswith(('build', 'twister-out')) for p in parts):
                continue
            yield path


def functions(lines, sections):
    """Names of the functions declared or defined in the given sections, in
    order, with the line each one starts on."""
    found = []
    section = None
    header = []
    in_body = False

    for number, line in enumerate(lines, 1):
        banner = BANNER.match(line)
        if banner:
            section = banner.group(1).strip()
            header = []
            continue
        if section not in sections:
            continue
        if in_body:
            in_body = not line.startswith('}')
            continue
        if not header and (not line or line[0] in ' \t#/*{}'):
            continue
        header.append((number, line))
        text = ' '.join(part for _, part in header)
        if line.rstrip().endswith(';') or line.startswith('{'):
            match = NAME.search(text)
            if match and not match.group(1).isupper():
                found.append((match.group(1), header[0][0]))
            in_body = line.startswith('{')
            header = []
    return found


def interface_header(path):
    if path.name == 'ubi_api.c':
        return ROOT / 'include' / 'ubi' / 'ubi.h'
    header = path.with_suffix('.h')
    return header if header.exists() else None


def order_check(path, lines, report):
    declared = functions(lines, (STATIC_DECLARATIONS,))
    defined = functions(lines, (STATIC_DEFINITIONS,))
    if [n for n, _ in declared] != [n for n, _ in defined]:
        where = defined[0][1] if defined else 1
        report(path, where, 'static functions are not defined in the order '
               'they are declared: declared %s, defined %s' %
               ([n for n, _ in declared], [n for n, _ in defined]))

    header = interface_header(path)
    if header is None:
        return
    header_lines = header.read_text(encoding='utf-8').splitlines()
    promised = [n for n, _ in functions(header_lines, INTERFACE_DECLARATIONS)]
    kept = [(n, at) for n, at in functions(lines, (INTERFACE_DEFINITIONS,))
            if n in promised]
    expected = [n for n in promised if n in {k for k, _ in kept}]
    if [n for n, _ in kept] != expected:
        report(path, kept[0][1], 'functions are not defined in the order %s '
               'declares them: expected %s' %
               (header.relative_to(ROOT), expected))


def main():
    failures = []

    def report(path, line, message):
        failures.append('%s:%d: %s' % (path.relative_to(ROOT), line, message))

    for path in sources():
        text = path.read_text(encoding='utf-8')
        lines = text.splitlines()
        top = path.relative_to(ROOT).parts[0]

        if len(lines) >= LINE_LIMIT:
            report(path, len(lines), '%d lines, split it below %d' %
                   (len(lines), LINE_LIMIT))

        for number, line in enumerate(lines, 1):
            if VOID_CAST.search(line):
                report(path, number, 'a result is cast to (void)')
            if top in ('tests', 'samples') and 'ztest_test_skip' in line:
                report(path, number, 'a test is skipped')

        if path.suffix == '.c':
            order_check(path, lines, report)

    for failure in failures:
        print(failure)

    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
