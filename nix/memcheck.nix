{
  lib,
  stdenv,
  python3,
  valgrind,
  src,
}:

let
  testRequirements = (lib.importTOML (src + "/tests/pyproject.toml")).project.dependencies;
  testPackageNames = map (
    requirement: builtins.head (builtins.match "([A-Za-z0-9_.-]+).*" requirement)
  ) (lib.filter (requirement: !(lib.hasInfix "python_version <" requirement)) testRequirements);
  python = python3.withPackages (
    packages: [ packages.setuptools ] ++ map (name: packages.${name}) testPackageNames
  );
in
stdenv.mkDerivation {
  pname = "salix-memcheck";
  version = (lib.importTOML (src + "/pyproject.toml")).project.version;
  inherit src;

  nativeBuildInputs = [
    python
    valgrind
  ];

  SALIX_STRICT = "1";

  dontConfigure = true;
  dontInstall = true;

  buildPhase = ''
    runHook preBuild
    set -o pipefail

    python setup.py build_ext --inplace
    export PYTHONPATH="$PWD:$(python -c 'import sys; print(":".join(path for path in sys.path if path))')"
    export PYTHONMALLOC=malloc
    ulimit -s 8192

    valgrind \
      --tool=memcheck \
      --main-stacksize=$(($(ulimit -s) * 1024)) \
      --trace-children=yes \
      --errors-for-leak-kinds=none \
      --error-exitcode=99 \
      python -m pytest -q -p no:cacheprovider | tee $out

    runHook postBuild
  '';

  meta.platforms = lib.platforms.linux;
}
