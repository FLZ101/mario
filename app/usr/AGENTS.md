# Third-Party Build Convention

All third-party libraries and applications in `app/usr` must follow the build
style of `build-ncurses.sh`. Do not introduce bespoke build-script conventions.
Opt Mario-native build targets into `COMPILE_COMMANDS_JSON` in `Makefile`.
Third-party builds do not record compile commands.

Keep C++ runtime and probe recipes, build directories, and staging outputs
under `app/usr/cxx/`. Keep installable app sources in `app/usr-cxx/` and test
programs in `app/test-cxx/`, with shared rules in `app/Makefile.cxx.in`.

## Build Tools

Use the project's build virtual environment for Python, Meson, Ninja, and other
Python-installable build tools. Declare their dependencies in `requirements.txt`
rather than requiring system packages or installing them from build scripts.

Use the `PYTHON` supplied by the build and resolve tool executables next to it
so recipes work with both host and container venvs. Pass those paths to build
systems that invoke the tools indirectly. If a required tool is missing, fail
with instructions to run `make venv` instead of falling back to a system copy.
