"""Build-local fixes required by the ESP8266 Arduino core 3.1.2.

Large firmware images can place ``postmortem_report`` outside the range of the
inline assembler's plain ``j`` instruction.  Upstream changed it to ``j.l``:
https://github.com/esp8266/Arduino/blob/521ae60a89e64bb0d1eb7a0b7addf620ced5cad3/cores/esp8266/core_esp8266_postmortem.cpp

The core also overwrites an explicitly configured ``WiFiClientSecure`` timeout
inside ``_freeSSL()`` and after the TLS handshake.  This makes the cooperative
SPA loop block for 15 seconds during a failed handshake and for 5 seconds on
later TLS I/O.  The local patch preserves caller-selected shorter timeouts
while retaining the core defaults for clients which do not configure one.

The installed PlatformIO package is never modified.  Build middleware replaces
only the affected translation units with patched copies under ``$BUILD_DIR``.  An
unknown source variant fails fast rather than applying a fuzzy patch.
"""

from pathlib import Path
import os
import sys
import tempfile

Import("env")


SOURCE_NAME = "core_esp8266_postmortem.cpp"
OLD_JUMP = '"j            postmortem_report\\n\\t"'
LONG_JUMP = '"j.l          postmortem_report, a3\\n\\t"'

SECURE_CLIENT_SOURCE_NAME = "WiFiClientSecureBearSSL.cpp"
RESET_TIMEOUT_BLOCK = "  _handshake_done = false;\n  _timeout = 15000;\n}"
PRESERVE_TIMEOUT_BLOCK = (
    "  _handshake_done = false;\n"
    "  // Preserve an explicitly configured Stream timeout across reconnects.\n"
    "}"
)
POST_HANDSHAKE_TIMEOUT = "  if (ret) _timeout = 5000;"
BOUNDED_POST_HANDSHAKE_TIMEOUT = (
    "  if (ret && _timeout > 5000) _timeout = 5000;"
)


def _write_if_changed(destination: Path, content: str) -> None:
    if destination.exists() and destination.read_text(encoding="utf-8") == content:
        return

    destination.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=destination.name + ".", suffix=".tmp", dir=str(destination.parent)
    )
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="") as output:
            output.write(content)
        os.replace(temporary_name, destination)
    except Exception:
        try:
            os.unlink(temporary_name)
        except FileNotFoundError:
            pass
        raise


def replace_postmortem_source(build_env, node):
    source = Path(node.srcnode().get_abspath())
    content = source.read_text(encoding="utf-8")

    old_occurrences = content.count(OLD_JUMP)
    long_occurrences = content.count(LONG_JUMP)
    if long_occurrences == 1 and old_occurrences == 0:
        print("ESP8266 postmortem: framework already contains upstream long jump")
        return node

    if old_occurrences != 1 or long_occurrences != 0:
        sys.stderr.write(
            "ESP8266 postmortem workaround refused to patch an unexpected "
            f"framework source ({source}, old jump occurrences: {old_occurrences}, "
            f"long jump occurrences: {long_occurrences}).\n"
        )
        build_env.Exit(1)

    patched = content.replace(OLD_JUMP, LONG_JUMP, 1)
    destination = (
        Path(build_env.subst("$BUILD_DIR")) / "patched_framework" / SOURCE_NAME
    )
    _write_if_changed(destination, patched)
    print(f"ESP8266 postmortem: using build-local upstream long-jump fix ({destination})")
    return build_env.File(str(destination))


def preserve_secure_client_timeout(build_env, node):
    source = Path(node.srcnode().get_abspath())
    content = source.read_text(encoding="utf-8")

    reset_occurrences = content.count(RESET_TIMEOUT_BLOCK)
    post_occurrences = content.count(POST_HANDSHAKE_TIMEOUT)
    if reset_occurrences != 1 or post_occurrences != 1:
        sys.stderr.write(
            "ESP8266 TLS timeout workaround refused to patch an unexpected "
            f"framework source ({source}, reset occurrences: {reset_occurrences}, "
            f"post-handshake occurrences: {post_occurrences}).\n"
        )
        build_env.Exit(1)

    patched = content.replace(RESET_TIMEOUT_BLOCK, PRESERVE_TIMEOUT_BLOCK, 1)
    patched = patched.replace(
        POST_HANDSHAKE_TIMEOUT, BOUNDED_POST_HANDSHAKE_TIMEOUT, 1
    )
    destination = (
        Path(build_env.subst("$BUILD_DIR"))
        / "patched_framework"
        / SECURE_CLIENT_SOURCE_NAME
    )
    _write_if_changed(destination, patched)
    print(
        "ESP8266 TLS: preserving caller-selected bounded timeout "
        f"({destination})"
    )
    return build_env.File(str(destination))


env.AddBuildMiddleware(replace_postmortem_source, f"*{SOURCE_NAME}")
env.AddBuildMiddleware(
    preserve_secure_client_timeout, f"*{SECURE_CLIENT_SOURCE_NAME}"
)
