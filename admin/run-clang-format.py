import os
import shutil
import subprocess
import sys


def main():
    # Optional explicit override for a particular machine.
    clang_format = os.environ.get("CLANG_FORMAT")

    if not clang_format:
        clang_format = shutil.which("clang-format-22")

    if not clang_format:
        clang_format = shutil.which("clang-format")

    if not clang_format:
        print(
            "clang-format not found "
            "(tried clang-format-22 and clang-format)",
            file=sys.stderr,
        )
        return 1

    return subprocess.call(
        [clang_format, "-i", *sys.argv[1:]]
    )


if __name__ == "__main__":
    sys.exit(main())
