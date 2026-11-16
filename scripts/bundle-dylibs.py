#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Copy a capture executable's non-system dylibs and rewrite links before codesigning."""

import platform
from pathlib import Path
import re
import shutil
import subprocess
import sys

SYSTEM_LOCATIONS = ("/usr/lib/", "/System/Library/")


def macho(path):
    """Return dylib dependencies, runpaths and deployment floor for the build architecture."""
    output = subprocess.check_output(["otool", "-arch", platform.machine(), "-l", path], text=True)
    dependencies = []
    runpaths = []
    minimum = (0,)
    for command in re.split(r"^Load command \d+\n", output, flags=re.M)[1:]:
        kind = re.search(r"^\s*cmd (\S+)", command, re.M).group(1)
        if kind in ("LC_LOAD_DYLIB", "LC_LOAD_WEAK_DYLIB", "LC_REEXPORT_DYLIB", "LC_LOAD_UPWARD_DYLIB"):
            dependencies.append(re.search(r"^\s*name (.+) \(offset \d+\)", command, re.M).group(1))
        elif kind == "LC_RPATH":
            runpaths.append(re.search(r"^\s*path (.+) \(offset \d+\)", command, re.M).group(1))
        elif kind in ("LC_BUILD_VERSION", "LC_VERSION_MIN_MACOSX"):
            version = re.search(r"^\s*(?:minos|version) ([\d.]+)", command, re.M).group(1)
            minimum = tuple(map(int, version.split(".")))
    return dependencies, runpaths, minimum


def expand(name, owner, executable):
    return Path(name.replace("@loader_path", str(owner.parent), 1)
                .replace("@executable_path", str(executable.parent), 1))


def bundle(executable, frameworks, search_paths):
    """Follow the load graph using original paths, then make the package independent of them."""
    copied = {}
    minimum = (14, 0)

    def relocate(source, output, inherited):
        nonlocal minimum
        dependencies, runpaths, version = macho(source)
        minimum = max(minimum, version)
        paths = [expand(path, source, executable) for path in runpaths] + inherited
        arguments = []
        for dependency in dependencies:
            if dependency.startswith(SYSTEM_LOCATIONS):
                continue
            if dependency.startswith("@rpath/"):
                relative = dependency.removeprefix("@rpath/")
                candidates = [path / relative for path in paths + search_paths]
            else:
                candidates = [expand(dependency, source, executable)]
            loaded = next((path for path in candidates if path.is_file()), None)
            if loaded is None or loaded.suffix != ".dylib":
                raise ValueError(f"Cannot bundle {dependency} loaded by {source}")
            real_path = loaded.resolve()
            if real_path not in copied:
                destination = frameworks / loaded.name
                if destination.exists():
                    raise ValueError(f"Conflicting library name: {destination.name}")
                shutil.copyfile(real_path, destination)
                # Reserve the destination before walking dependencies that may refer back to it.
                copied[real_path] = destination.name
                relocate(loaded, destination, paths)
            prefix = "@loader_path"
            if output == executable:
                prefix += "/../Frameworks"
            arguments.extend(("-change", dependency, f"{prefix}/{copied[real_path]}"))

        # The rewritten links are direct. Keep build-machine runpaths out of the install.
        for path in runpaths:
            arguments.extend(("-delete_rpath", path))
        if output != executable:
            arguments.extend(("-id", f"@rpath/{output.name}"))
        if arguments:
            # Remove the stale signature only from the bundle copy before rewriting its load commands.
            subprocess.run(["codesign", "--remove-signature", output], check=True)
            subprocess.run(["install_name_tool", *arguments, output], check=True)

    relocate(executable, executable, [])
    # Verify the sealed bundle can't silently fall back to a build-machine copy.
    for path in [executable, *(frameworks / name for name in copied.values())]:
        dependencies, runpaths, _ = macho(path)
        if runpaths:
            raise ValueError(f"Unexpected runpaths in {path}")
        for dependency in dependencies:
            if not dependency.startswith(SYSTEM_LOCATIONS) and (
                not dependency.startswith("@loader_path/") or not expand(dependency, path, executable).is_file()
            ):
                raise ValueError(f"Unresolved bundled link {dependency} in {path}")
    return ".".join(map(str, minimum))


if __name__ == "__main__":
    executable, frameworks, *search_paths = map(Path, sys.argv[1:])
    try:
        print(bundle(executable, frameworks, search_paths))
    except (ValueError, subprocess.CalledProcessError) as error:
        sys.exit(str(error))
