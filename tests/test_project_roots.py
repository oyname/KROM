"""Exercise the production CMake project discovery without building the engine.

Run: python tests/test_project_roots.py --cmake <cmake.exe> --ninja <ninja.exe>
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--ninja", default="ninja")
    args = parser.parse_args()
    module = Path(__file__).resolve().parents[1] / "cmake/KromProjects.cmake"
    with tempfile.TemporaryDirectory(prefix="krom-projects-Größe ") as scratch:
        root = Path(scratch)
        source, build = root / "engine", root / "build"
        source.mkdir()
        (source / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.22)\nproject(ProjectRoots NONE)\n'
            f'include([[{module.as_posix()}]])\n'
            'krom_collect_projects(roots)\n'
            'file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/roots.txt" CONTENT "${roots}")\n'
            'file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/selected.txt" CONTENT "${KROM_PROJECT_ROOT}")\n',
            encoding="utf-8",
        )

        def project(path, target):
            path.mkdir(parents=True, exist_ok=True)
            (path / "krom-project.json").write_text(
                json.dumps({"name": path.name, "runtimeTarget": target}), encoding="utf-8"
            )
            return path.resolve()

        demo = project(source / "projects/DemoProject", "DemoProject")
        a = project(root / "Spiele Größe A", "ExternalA")
        b = project(root / "Spiele B", "ExternalB")

        def configure(options, ok=True, directory=build):
            result = subprocess.run(
                [args.cmake, "-S", str(source), "-B", str(directory), "-G", "Ninja",
                 f"-DCMAKE_MAKE_PROGRAM={args.ninja}", *options],
                capture_output=True, encoding="utf-8", errors="replace",
            )
            assert (result.returncode == 0) == ok, result.stdout + result.stderr
            return result.stdout + result.stderr

        def selected(path):
            configure([f"-DKROM_PROJECT_ROOT={path}"])
            roots = (build / "roots.txt").read_text(encoding="utf-8").split(";")
            assert roots == [demo.as_posix(), path.as_posix()], roots

        selected(a)
        project(demo, "ExternalA")
        regenerated = subprocess.run(
            [args.cmake, "--build", str(build)], capture_output=True, encoding="utf-8", errors="replace"
        )
        assert regenerated.returncode != 0 and "target collision" in regenerated.stdout + regenerated.stderr
        project(demo, "DemoProject")
        selected(b)
        configure([f"-DKROM_PROJECT_ROOT={demo}/../DemoProject"])
        assert (build / "roots.txt").read_text(encoding="utf-8") == demo.as_posix()
        # A Windows junction represents the same physical directory through another path.
        alias = root / "Demo alias"
        if os.name == "nt":
            command = f'mklink /J "{alias}" "{demo}"'
            made = subprocess.run(["cmd", "/c", command], capture_output=True).returncode == 0
        else:
            alias.symlink_to(demo, target_is_directory=True)
            made = True
        if made:
            try:
                configure([f"-DKROM_PROJECT_ROOT={alias}"])
                assert (build / "roots.txt").read_text(encoding="utf-8") == demo.as_posix()
            finally:
                # Remove only the link, never recursively traverse the linked directory.
                if os.name == "nt":
                    alias.rmdir()
                else:
                    alias.unlink()

        collision = project(root / "collision", "DemoProject")
        error = configure([f"-DKROM_PROJECT_ROOT={collision}"], ok=False)
        assert "target collision 'DemoProject'" in error and "runtimeTarget" in error
        compact = " ".join(error.split())
        assert demo.as_posix() in compact and collision.as_posix() in compact
        assert (build / "selected.txt").read_text(encoding="utf-8") == demo.as_posix()
        selected(a)  # Recover from the failed configure in the same tree.
        project(source / "projects/Sanitized", "A-B")
        project(collision, "A_B")
        error = configure([f"-DKROM_PROJECT_ROOT={collision}"], ok=False)
        assert "target collision 'A_B'" in error
        warning = configure([f"-DKROM_PROJECT_ROOT={root / 'missing'}"])
        assert "Ignoring KROM_PROJECT_ROOT" in warning
        assert (build / "selected.txt").read_text(encoding="utf-8") == ""
        # Legacy migration, explicit new-name precedence, and clearing stale selection.
        legacy = root / "legacy-build"
        warning = configure([f"-DKROM_PROJECT_SCRIPT_ROOT={b}"], directory=legacy)
        assert "deprecated" in warning
        assert (legacy / "selected.txt").read_text(encoding="utf-8") == b.as_posix()
        warning = configure([f"-DKROM_PROJECT_SCRIPT_ROOT={a}"], directory=legacy)
        assert "ignored" in warning
        assert (legacy / "selected.txt").read_text(encoding="utf-8") == b.as_posix()
        warning = configure([f"-DKROM_PROJECT_ROOT={a}"], directory=legacy)
        assert "deprecated" not in warning  # Unchanged legacy mirror is not a supplied override.
        configure(["-DKROM_PROJECT_ROOT=", f"-DKROM_PROJECT_SCRIPT_ROOT={b}"], directory=legacy)
        assert (legacy / "selected.txt").read_text(encoding="utf-8") == ""
        configure([], directory=legacy)
        assert (legacy / "selected.txt").read_text(encoding="utf-8") == ""
        movable = project(root / "movable", "Movable")
        configure([f"-DKROM_PROJECT_ROOT={movable}"])
        moved = root / "renamed"
        movable.rename(moved)
        # Automatic regeneration must also recover without a manual -D argument.
        regenerated = subprocess.run(
            [args.cmake, "--build", str(build)], capture_output=True, encoding="utf-8", errors="replace"
        )
        assert regenerated.returncode == 0, regenerated.stdout + regenerated.stderr
        assert "Ignoring KROM_PROJECT_ROOT" in regenerated.stdout + regenerated.stderr
        assert (build / "selected.txt").read_text(encoding="utf-8") == ""
        cache = (build / "CMakeCache.txt").read_text(encoding="utf-8")
        assert "KROM_PROJECT_ROOT:PATH=\n" in cache and "KROM_PROJECT_SCRIPT_ROOT:PATH=\n" in cache
        configure([f"-DKROM_PROJECT_ROOT={moved}"])
        (moved / "krom-project.json").unlink()
        moved.rmdir()  # This fixture contains only its manifest, inside our temporary directory.
        configure([])
        assert (build / "selected.txt").read_text(encoding="utf-8") == ""
        configure([])  # Cleared aliases must not resurrect the missing selection.
        assert (build / "selected.txt").read_text(encoding="utf-8") == ""
        print("PASS: Unicode paths, canonical deduplication, collisions, moved/deleted projects, recovery and legacy diagnostics")


if __name__ == "__main__":
    main()
