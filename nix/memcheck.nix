{
  lib,
  stdenv,
  python3,
  valgrind,
  src,
}:

let
  python = python3.withPackages (packages: [
    packages.setuptools
    packages.pytest
    packages.hypothesis
    packages.packaging
  ]);
in
stdenv.mkDerivation {
  pname = "salix-memcheck";
  version = (lib.importTOML (src + "/pyproject.toml")).project.version;
  inherit src;

  nativeBuildInputs = [
    python
    valgrind
  ];

  dontConfigure = true;
  dontInstall = true;

  buildPhase = ''
    runHook preBuild

    python setup.py build_ext --inplace
    export PYTHONPATH="$PWD:$(python -c 'import sys; print(":".join(path for path in sys.path if path))')"
    export PYTHONMALLOC=malloc
    stackKib=8192
    ulimit -s $stackKib

    valgrind \
      --tool=memcheck \
      --main-stacksize=$((stackKib * 1024)) \
      --trace-children=yes \
      --errors-for-leak-kinds=none \
      --error-exitcode=99 \
      python -m pytest -q -p no:cacheprovider tests > $out

    runHook postBuild
  '';
}
