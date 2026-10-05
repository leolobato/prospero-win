# SPDX-License-Identifier: LGPL-2.1-or-later
"""Strict Windows user binding shared by bottle preparation and packaging."""
from __future__ import annotations

import re


def windows_user_bound(registry: str, user: str) -> bool:
    # Count effective headers before checking syntax; a malformed duplicate
    # must not hide a second binding from the validator.
    headers = list(re.finditer(r"^\[Volatile Environment\]", registry,
                               re.MULTILINE | re.IGNORECASE))
    if len(headers) != 1 or not re.fullmatch(r"[A-Za-z0-9_-]+", user):
        return False
    header = headers[0]
    newline = registry.find("\n", header.end())
    if newline < 0 or not re.fullmatch(r"(?: [0-9]+)?\r?", registry[header.end():newline]):
        return False
    end = registry.find("\n[", newline)
    body = registry[newline + 1:] if end < 0 else registry[newline + 1:end]
    expected = {"USERNAME": user, "USERPROFILE": rf"C:\\users\\{user}"}
    for key, value in expected.items():
        assignments = [line.strip() for line in body.splitlines()
                       if line.strip().lower().startswith(f'"{key}"='.lower())]
        if len(assignments) != 1 or assignments[0].lower() != f'"{key}"="{value}"'.lower():
            return False
    return True
