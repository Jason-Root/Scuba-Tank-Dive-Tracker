Import("env")

import subprocess
from pathlib import Path


def get_git_version():
    project_dir = Path(env.subst("$PROJECT_DIR"))
    try:
        return subprocess.check_output(
            ["git", "describe", "--tags", "--always", "--dirty"],
            cwd=project_dir,
            stderr=subprocess.DEVNULL,
            text=True,
        ).strip()
    except Exception:
        return "unknown"


git_version = get_git_version().replace('"', "")
env.Append(BUILD_FLAGS=[f'-DFIRMWARE_VERSION_STR=\\"{git_version}\\"'])
