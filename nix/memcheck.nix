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
    process_logs="$TMPDIR/memcheck"
    mkdir "$process_logs"

    pytest_status=0
    valgrind \
      --tool=memcheck \
      --main-stacksize=$(($(ulimit -s) * 1024)) \
      --trace-children=yes \
      --leak-check=full \
      --show-leak-kinds=definite,indirect \
      --errors-for-leak-kinds=definite \
      --log-file="$process_logs/%p.log" \
      python -m pytest -q -p no:cacheprovider | tee $out || pytest_status=$?

    cat "$process_logs"/*.log | tee -a $out
    processes_with_errors=$(grep --files-without-match "ERROR SUMMARY: 0 errors" "$process_logs"/*.log || true)
    if [ -n "$processes_with_errors" ]; then
      echo "memcheck reported errors in: $processes_with_errors" >&2
      exit 1
    fi
    [ "$pytest_status" -eq 0 ]

    runHook postBuild
  '';

  meta.platforms = lib.platforms.linux;
}
