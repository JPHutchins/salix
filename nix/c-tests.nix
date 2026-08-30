# The in-file unit tests, built and run.
#
# These embed CPython rather than mocking it: the subject is C that speaks to
# the C API, so a fake PyObject would only be testing the fake. That is also why
# this is a nix derivation and not a bare camas command -- it needs libpython
# and unity on the link line, and nix is where those paths come from.
{
  lib,
  stdenv,
  python3,
  unity-test,
  src,
}:

stdenv.mkDerivation {
  pname = "salix-c-tests";
  version = (lib.importTOML (src + "/pyproject.toml")).project.version;
  inherit src;

  nativeBuildInputs = [ python3 ];

  dontConfigure = true;
  dontInstall = true;

  buildPhase = ''
    runHook preBuild

    mapfile -t cFlags < <(python3 build_config.py c-flags --strict)
    mapfile -t sources < <(python3 build_config.py sources)

    $CC -DTESTING \
      "''${cFlags[@]}" \
      $(python3-config --includes) \
      -I${unity-test.dev}/include/unity \
      "''${sources[@]}" src/testing.c tests/c/main.c \
      -L${unity-test}/lib -lunity \
      $(python3-config --ldflags --embed) \
      -o c-tests

    ./c-tests > $out

    runHook postBuild
  '';
}
