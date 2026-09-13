"""Credential redaction for log and error text; mirrors fovea::redactText in the core."""
from __future__ import annotations

import re

URL_CREDENTIALS_RE = re.compile(r"(://)([^/@\s]+)@")
JSON_PASSWORD_RE = re.compile(r'("password"\s*:\s*")[^"]*(")')


def redact_text(text: str) -> str:
    return JSON_PASSWORD_RE.sub(r"\1***\2", URL_CREDENTIALS_RE.sub(r"\1***@", text))
