#!/usr/bin/env python3
"""Compile every Ragtime source with GCC 16 and -Werror, at -O2 and -O3.

CI does this too (the gcc16 job), but this runs where apt can't reach
Debian's mirrors, such as the Claude cloud sandbox: it configures a host
build for its compile commands, then replays them in the gcc:16 Docker
image with the host's headers mounted read-only, so wxWidgets is found
without installing anything in the container.

    dockerd &                      # if the daemon isn't running
    tools/gcc16/check.py           # from the repository root

Needs docker, cmake, a host compiler and the host's wxWidgets headers
(libwxgtk3.2-dev). Pass -DRAGTIME_APP=OFF and friends after `--` to
change the configure step.
"""

import json
import os
import re
import shlex
import subprocess
import sys

IMAGE = "gcc:16"
MULTIARCH = "/usr/lib/x86_64-linux-gnu"


def main():
    root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    build = os.path.join(root, "build-gcc16-commands")
    extra = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []

    subprocess.run(["cmake", "-S", root, "-B", build, "-DCMAKE_BUILD_TYPE=Release",
                    "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON", "-DRAGTIME_WERROR=ON"] + extra,
                   check=True, stdout=subprocess.DEVNULL)
    # Build once on the host so any generated sources exist. Its errors are
    # ignored here: the GCC 16 pass below reports them.
    subprocess.run(["cmake", "--build", build, "-j", str(os.cpu_count() or 2)],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    with open(os.path.join(build, "compile_commands.json")) as f:
        commands = json.load(f)

    lines = ["set -e"]
    for opt in ("-O2", "-O3"):
        for c in commands:
            args = shlex.split(c["command"])
            args[0] = "g++"
            args = [re.sub(r"^-O[0-3s]$", opt, a) for a in args]
            # Objects go nowhere: this is a compile check, not a build.
            for i, a in enumerate(args):
                if a == "-o":
                    args[i + 1] = "/dev/null"
            cmd = " ".join(shlex.quote(a) for a in args)
            cmd = cmd.replace("/usr/include/", "/hostinc/").replace(MULTIARCH + "/", "/hostlib/")
            if "/hostinc/" in cmd or "/hostlib/" in cmd:
                cmd += " -idirafter /hostinc -idirafter /hostinc/x86_64-linux-gnu"
            name = os.path.relpath(c["file"], root)
            lines.append(f"cd {shlex.quote(c['directory'])} && echo {shlex.quote(opt + ' ' + name)} && {cmd}")
    lines.append("echo 'GCC 16 -Werror: all clean'")

    script = os.path.join(build, "gcc16.sh")
    with open(script, "w") as f:
        f.write("\n".join(lines) + "\n")

    result = subprocess.run(["docker", "run", "--rm",
                             "-v", f"{root}:{root}",
                             "-v", "/usr/include:/hostinc:ro",
                             "-v", f"{MULTIARCH}:/hostlib:ro",
                             IMAGE, "sh", script])
    return result.returncode


if __name__ == "__main__":
    sys.exit(main())
