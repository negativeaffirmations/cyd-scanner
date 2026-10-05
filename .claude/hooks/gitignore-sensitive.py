#!/usr/bin/env python3
"""PostToolUse hook (Write|Edit): keep machine-specific / sensitive files out of git.

Reads the hook JSON on stdin. If the file that was just written contains any of:
  * this machine's home-directory path, or any absolute user-home path
    (``C:\\Users\\<name>\\...``, ``/home/<name>/...``, ``/Users/<name>/...``);
  * a machine-specific absolute path (Windows ``X:\\...`` or Git-Bash ``/x/...``);
  * something that looks like an API key / secret / hard-coded credential;
then the file is added to the repo's .gitignore so it is never committed.

Detection is content-based on purpose: a platform-dependent helper that ships
with a package but embeds no machine path or secret is left alone; only files
that would actually break, leak a username, or leak a credential are ignored.

Fail-safe: never blocks the tool and never raises — any error just exits 0.
"""
import json
import os
import re
import subprocess
import sys

MAX_BYTES = 2_000_000  # don't scan very large / likely-binary files

# --- Absolute user-home paths (any user) ------------------------------------
HOME_PATH_RE = re.compile(
    r"(?:[A-Za-z]:[\\/]Users[\\/]|/[a-z]/Users/|/Users/|/home/)[A-Za-z0-9._-]+",
    re.IGNORECASE,
)

# --- Machine-specific absolute paths ----------------------------------------
# Windows drive path (E:\..., C:/...). Lookbehind avoids matching URL schemes
# like "http://" (the drive letter must NOT be preceded by another letter/digit).
WIN_DRIVE_RE = re.compile(r"(?<![A-Za-z0-9])[A-Za-z]:[\\/][^\r\n\"'<>|*?]{2,}")
# Git-Bash mounted drive (/e/Documents, /c/Users). Single-letter segment only.
POSIX_DRIVE_RE = re.compile(r"(?<![A-Za-z0-9])/[a-z]/[A-Za-z0-9._\-][A-Za-z0-9._\-/]+")

# --- Secrets / credentials --------------------------------------------------
PRIVATE_KEY_RE = re.compile(r"-----BEGIN (?:[A-Z0-9 ]+ )?PRIVATE KEY-----")
# High-confidence provider token formats.
SECRET_TOKEN_RE = re.compile(
    r"\b(?:"
    r"AKIA[0-9A-Z]{16}"                        # AWS access key id
    r"|gh[pousr]_[A-Za-z0-9]{30,}"             # GitHub tokens
    r"|github_pat_[A-Za-z0-9_]{22,}"           # GitHub fine-grained PAT
    r"|glpat-[A-Za-z0-9_\-]{20,}"              # GitLab PAT
    r"|AIza[0-9A-Za-z_\-]{35}"                 # Google API key
    r"|ya29\.[0-9A-Za-z_\-]{20,}"              # Google OAuth token
    r"|xox[baprs]-[0-9A-Za-z-]{10,}"           # Slack
    r"|[sr]k_(?:live|test)_[0-9A-Za-z]{16,}"   # Stripe
    r"|sk-[A-Za-z0-9]{20,}"                    # OpenAI-style
    r"|eyJ[A-Za-z0-9_\-]{8,}\.[A-Za-z0-9_\-]{8,}\.[A-Za-z0-9_\-]{8,}"  # JWT
    r")\b"
)
# Generic "<secret-name> = <long value>" assignments.
SECRET_ASSIGN_RE = re.compile(
    r"(?i)\b(?:api[_-]?key|secret|access[_-]?key|client[_-]?secret|auth[_-]?token"
    r"|token|password|passwd|passphrase|private[_-]?key)\b\s*[:=]\s*"
    r"[\"']?([A-Za-z0-9_\-\.+/]{16,})[\"']?"
)
# Values that are clearly NOT real secrets (references / placeholders).
PLACEHOLDER_RE = re.compile(
    r"(?i)(process\.env|os\.environ|getenv|ENV\[|\$\{?[A-Za-z_][A-Za-z0-9_]*\}?"
    r"|<[^>]+>|x{4,}|your[_-]|example|placeholder|changeme|redacted|dummy|sample"
    r"|todo|none|null|true|false|\.\.\.)"
)


def run_git(repo, *args):
    try:
        return subprocess.run(
            ["git", "-C", repo, *args],
            capture_output=True, text=True, timeout=10,
        )
    except Exception:
        return None


def repo_root(start_dir):
    out = run_git(start_dir, "rev-parse", "--show-toplevel")
    if out and out.returncode == 0:
        return out.stdout.strip()
    return None


def machine_tokens():
    """Exact machine-specific strings for THIS machine, derived (never hard-coded)."""
    toks = set()
    user = os.environ.get("USERNAME") or os.environ.get("USER")
    home = os.environ.get("HOME") or os.environ.get("USERPROFILE")
    if user and len(user) >= 2:
        toks.update({
            f"C:\\Users\\{user}", f"C:/Users/{user}",
            f"/c/Users/{user}", f"/Users/{user}", f"/home/{user}",
        })
    if home:
        toks.add(home)
        m = re.match(r"^/([a-zA-Z])/(.*)$", home)  # /c/Users/x -> C:\Users\x
        if m:
            drive, rest = m.group(1).upper(), m.group(2)
            toks.add(f"{drive}:\\{rest.replace('/', chr(92))}")
            toks.add(f"{drive}:/{rest}")
    return {t for t in toks if t}


def sensitivity_reason(content):
    """Return a short reason string if the content is sensitive, else None."""
    if any(t in content for t in machine_tokens()):
        return "a machine-specific home path"
    if HOME_PATH_RE.search(content):
        return "an absolute user-home path"
    if WIN_DRIVE_RE.search(content) or POSIX_DRIVE_RE.search(content):
        return "a machine-specific absolute path"
    if PRIVATE_KEY_RE.search(content) or SECRET_TOKEN_RE.search(content):
        return "what looks like an API key or secret"
    m = SECRET_ASSIGN_RE.search(content)
    if m and not PLACEHOLDER_RE.search(m.group(0)):
        return "what looks like a hard-coded credential"
    return None


def main():
    try:
        data = json.load(sys.stdin)
    except Exception:
        return

    ti = data.get("tool_input") or {}
    fp = ti.get("file_path") or ti.get("filePath")
    if not fp:
        return
    fp = os.path.abspath(fp)
    if not os.path.isfile(fp):
        return

    repo = repo_root(os.path.dirname(fp)) or repo_root(data.get("cwd") or ".")
    if not repo:
        return

    rel = os.path.relpath(fp, repo).replace(os.sep, "/")
    # Never touch the ignore file, git internals, or our own config/tooling
    # (this script contains path/secret patterns in its own source).
    if (rel.startswith("../") or rel == ".gitignore"
            or rel.startswith(".git/") or rel.startswith(".claude/")):
        return

    # Already tracked by git? Then .gitignore wouldn't stop it anyway (git keeps tracking
    # ignored-but-tracked files), so skip — this hook only keeps NEW/untracked files out.
    tracked = run_git(repo, "ls-files", "--error-unmatch", rel)
    if tracked is not None and tracked.returncode == 0:
        return

    try:
        if os.path.getsize(fp) > MAX_BYTES:
            return
        with open(fp, "r", encoding="utf-8", errors="ignore") as f:
            content = f.read()
    except Exception:
        return

    reason = sensitivity_reason(content)
    if not reason:
        return

    # Skip if git already ignores it (idempotent, no duplicate lines).
    chk = run_git(repo, "check-ignore", "-q", rel)
    if chk is not None and chk.returncode == 0:
        return

    gitignore = os.path.join(repo, ".gitignore")
    try:
        existing = ""
        if os.path.isfile(gitignore):
            with open(gitignore, "r", encoding="utf-8", errors="ignore") as f:
                existing = f.read()
        if any(line.strip() == rel for line in existing.splitlines()):
            return
        prefix = "" if (not existing or existing.endswith("\n")) else "\n"
        block = (
            f"{prefix}\n# Added automatically: contains {reason} "
            f"(sensitive / would break on another machine)\n{rel}\n"
        )
        with open(gitignore, "a", encoding="utf-8") as f:
            f.write(block)
    except Exception:
        return

    print(json.dumps({
        "systemMessage": (
            f"Added '{rel}' to .gitignore \u2014 it contains {reason} "
            f"(sensitive / non-portable). Remove the line if that was intended."
        )
    }))


if __name__ == "__main__":
    try:
        main()
    except Exception:
        pass
