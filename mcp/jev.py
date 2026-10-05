"""Jev — TypeSafe's structured decision model — for the one closed choice Kelvin needs a judge
for: which column of a BOM a header names, when the header is not in bomfile's vocabulary.

Jev (``typesafe/jev-1.13`` on OpenRouter's decisions endpoint) never writes text: it reads a
``state`` and answers typed questions. Here every question is a ``choice`` whose options are a
closed list of BOM roles plus ``ignore``, so an answer can only ever name a role this parser
knows. Same client shape as Heaviside's heaviside/llm/jev.py and the machine's jevlib.

The key: MOEBIUS_JEV_API_KEY (what /cache/moebius/jev.env carries on prod) or
OPENROUTER_API_KEY. No key, an HTTP failure after retries, or a missing answer raises
JevError — a column mapping nobody made must never look like one that was made.
"""

from __future__ import annotations

import json
import os
import time
import urllib.error
import urllib.request

JEV_URL = "https://openrouter.ai/api/alpha/decisions"
JEV_MODEL = "typesafe/jev-1.13"
KEY_VARS = ("MOEBIUS_JEV_API_KEY", "OPENROUTER_API_KEY")
MAX_STATE_CHARS = 100_000
MAX_QUESTIONS_PER_CALL = 64


class JevError(RuntimeError):
    """A Jev decision could not be obtained (no key, HTTP error, bad answer)."""


def api_key() -> str:
    for var in KEY_VARS:
        key = os.environ.get(var, "").strip()
        if key:
            return key
    raise JevError(f"no Jev key: set {' or '.join(KEY_VARS)} in the Kelvin MCP server's "
                   f"environment")


def _post(body: dict, *, timeout: float = 60.0, retries: int = 3) -> dict:
    key = api_key()
    url = os.environ.get("KELVIN_JEV_URL", JEV_URL)
    last = ""
    for attempt in range(retries + 1):
        req = urllib.request.Request(
            url, data=json.dumps(body).encode(), method="POST",
            headers={"Authorization": f"Bearer {key}", "Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                return json.loads(resp.read())
        except urllib.error.HTTPError as exc:
            last = f"HTTP {exc.code}: {exc.read()[:300].decode('utf-8', 'replace')}"
            if exc.code not in (408, 429) and exc.code < 500:
                break                       # a request error will not fix itself on retry
        except (urllib.error.URLError, TimeoutError, OSError, ValueError) as exc:
            last = f"{type(exc).__name__}: {exc}"
        if attempt < retries:
            time.sleep(min(2 ** attempt, 8))
    raise JevError(f"Jev decision failed ({last})")


def decide(state, questions: dict[str, dict]) -> dict[str, dict]:
    """Ask every question about ``state``; ``{question_id: answer}``. Raises JevError."""
    if not questions:
        return {}
    size = len(state) if isinstance(state, str) else len(json.dumps(state, default=str))
    if size > MAX_STATE_CHARS:
        raise JevError(f"Jev state is {size} characters; its context holds ~{MAX_STATE_CHARS}")
    ids = list(questions)
    answers: dict[str, dict] = {}
    for i in range(0, len(ids), MAX_QUESTIONS_PER_CALL):
        chunk = {k: questions[k] for k in ids[i:i + MAX_QUESTIONS_PER_CALL]}
        data = _post({"model": os.environ.get("KELVIN_JEV_MODEL", JEV_MODEL),
                      "state": state, "questions": chunk})
        got = data.get("answers") or {}
        missing = [k for k in chunk if k not in got]
        if missing:
            raise JevError(f"Jev returned no answer for {missing}")
        answers.update(got)
    return answers
