"""PlatformIO pre-build script: point the dashboard footer at the repo that built it.

Reads `git remote get-url origin` and `git rev-parse --short HEAD`, turns the
remote into a plain https link (SSH form converted, any user or token in the
URL dropped, ".git" trimmed), and passes both to the firmware as
IHEATER_GIT_URL and IHEATER_GIT_COMMIT. Anything that does not look like a
safe https URL falls back to the upstream repository.
"""

import re
import subprocess

Import("env")  # noqa: F821 (provided by PlatformIO/SCons)

FALLBACK_URL = "https://github.com/justinh-rahb/iHeater-Remote"
SAFE_URL = re.compile(r"^https://[A-Za-z0-9.-]+(:[0-9]+)?(/[A-Za-z0-9._~-]+)+$")


def git(*args):
    try:
        out = subprocess.run(
            ["git", *args],
            cwd=env.subst("$PROJECT_DIR"),  # noqa: F821
            capture_output=True,
            text=True,
            timeout=10,
        )
    except (OSError, subprocess.SubprocessError):
        return ""
    return out.stdout.strip() if out.returncode == 0 else ""


def web_url(remote):
    url = remote.strip()
    # git@host:owner/repo(.git) -> https://host/owner/repo
    m = re.match(r"^[A-Za-z0-9._-]+@([A-Za-z0-9.-]+):(.+)$", url)
    if m:
        url = "https://%s/%s" % (m.group(1), m.group(2))
    # ssh://git@host[:port]/owner/repo -> https://host/owner/repo (an SSH port
    # is not the web port).
    m = re.match(r"^(ssh|git)://(?:[^/@]*@)?([A-Za-z0-9.-]+)(?::[0-9]+)?/(.+)$", url)
    if m:
        url = "https://%s/%s" % (m.group(2), m.group(3))
    # Drop any user, password or token before the host.
    url = re.sub(r"^https?://[^/@]*@", "https://", url)
    url = re.sub(r"^http://", "https://", url)
    url = re.sub(r"\.git/?$", "", url).rstrip("/")
    return url if SAFE_URL.match(url) else FALLBACK_URL


url = web_url(git("remote", "get-url", "origin"))
commit = git("rev-parse", "--short", "HEAD")
if commit and git("status", "--porcelain", "--untracked-files=no"):
    commit += "-dirty"
if not re.match(r"^[0-9a-f]{4,40}(-dirty)?$", commit):
    commit = ""

print("Footer link: %s (%s)" % (url, commit or "no commit"))
env.Append(  # noqa: F821
    CPPDEFINES=[
        ("IHEATER_GIT_URL", env.StringifyMacro(url)),  # noqa: F821
        ("IHEATER_GIT_COMMIT", env.StringifyMacro(commit)),  # noqa: F821
    ]
)
