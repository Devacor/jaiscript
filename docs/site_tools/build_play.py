"""Builds what the site's Play pages load and git does not track: the engine as WebAssembly (tools/jaiscript_web, built
with Emscripten) and one bundle per example game (its .jai files and arguments as JSON).

    python docs/site_tools/build_play.py [--emsdk DIR] [--site DIR] [--build DIR] [--games-only]

--emsdk defaults to $EMSDK, --site to docs/site, --build to out/build/wasm-site (the threaded worker builds in
<build>-threads). Writes <site>/play/engine/ (the landing page's scene loads engine/plain/) and <site>/play/games/.
"""
import argparse
import glob
import json
import os
import pathlib
import shutil
import subprocess
import sys

PACKAGE = pathlib.Path(__file__).resolve().parent.parent.parent

# id: (folder under examples/, entry script relative to it, globs of the files it loads, its ARGS)
GAMES = {
    "gloom": ("gloom", "gloom.jai", ["gloom.jai", "scripts/*.jai"], []),
    "grubwell": ("crawler", "scripts/crawler.jai", ["scripts/*.jai"], []),
    "demoreel": ("demoreel", "demoreel.jai", ["demoreel.jai", "scenes/**/*.jai"], []),
}


def emscripten_environment(emsdk):
    root = pathlib.Path(emsdk).resolve()
    emscripten = root / "upstream" / "emscripten"
    toolchain = emscripten / "cmake" / "Modules" / "Platform" / "Emscripten.cmake"
    if not toolchain.exists():
        sys.exit("build_play: no Emscripten toolchain under %s (pass --emsdk or set EMSDK)" % root)
    env = dict(os.environ)
    env["EMSDK"] = str(root)
    env["EM_CONFIG"] = str(root / ".emscripten")
    paths = [str(emscripten)] + sorted(glob.glob(str(root / "node" / "*" / "bin")))
    env["PATH"] = os.pathsep.join(paths + [env.get("PATH", "")])
    return env, toolchain


def build_engine(emsdk, build, engine_dir):
    """The worker engine on threads (JAISCRIPT_WASM_THREADS), and in a tree without them the Asyncify fallback and the
    plain worker build the landing page runs on its main thread."""
    env, toolchain = emscripten_environment(emsdk)
    threaded = build.with_name(build.name + "-threads")
    for tree, threads, target, folder in [(threaded, "ON", "jaiscript_web_worker", engine_dir),
                                          (build, "OFF", "jaiscript_web", engine_dir),
                                          (build, "OFF", "jaiscript_web_worker", engine_dir / "plain")]:
        configure = ["cmake", "-S", str(PACKAGE), "-B", str(tree), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
                     "-DCMAKE_TOOLCHAIN_FILE=%s" % toolchain, "-DJAISCRIPT_WASM_THREADS=%s" % threads]
        for option in ["TESTS", "EXAMPLES", "RUNNER", "SPIDER", "LSP"]:
            configure.append("-DJAISCRIPT_BUILD_%s=OFF" % option)
        configure += ["-DJAISCRIPT_INTERPRETER=OFF", "-DJAISCRIPT_INSTALL=OFF"]
        subprocess.run(configure, check=True, env=env, stdout=subprocess.DEVNULL)
        subprocess.run(["cmake", "--build", str(tree), "--target", target], check=True, env=env)
        folder.mkdir(parents=True, exist_ok=True)
        for suffix in (".js", ".wasm"):
            shutil.copy2(tree / "bin" / (target + suffix), folder / (target + suffix))


def bundle_games(games_dir):
    games_dir.mkdir(parents=True, exist_ok=True)
    for game, (folder, entry, patterns, args) in GAMES.items():
        root = PACKAGE / "examples" / folder
        files = {}
        for pattern in patterns:
            for path in sorted(root.glob(pattern)):
                files[path.relative_to(root).as_posix()] = path.read_text(encoding="utf-8")
        if entry not in files:
            sys.exit("build_play: %s's entry %s is not in its bundle" % (game, entry))
        with (games_dir / (game + ".json")).open("w", encoding="utf-8", newline="\n") as handle:
            json.dump({"entry": entry, "args": args, "files": files}, handle, separators=(",", ":"))
        print("bundled %s: %d files" % (game, len(files)))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--emsdk", default=os.environ.get("EMSDK"))
    parser.add_argument("--site", default=str(PACKAGE / "docs" / "site"))
    parser.add_argument("--build", default=str(PACKAGE / "out" / "build" / "wasm-site"))
    parser.add_argument("--games-only", action="store_true", help="rebundle the games, keep the engine")
    args = parser.parse_args()
    play = pathlib.Path(args.site) / "play"
    if not args.games_only:
        if not args.emsdk:
            sys.exit("build_play: pass --emsdk or set EMSDK")
        build_engine(args.emsdk, pathlib.Path(args.build), play / "engine")
    bundle_games(play / "games")
    print("play pages ready under %s" % play)
    return 0


if __name__ == "__main__":
    sys.exit(main())
