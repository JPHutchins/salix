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
    mapfile -t testSources < <(python3 build_config.py test-sources)

    $CC -DTESTING \
      "''${cFlags[@]}" \
      $(python3-config --includes) \
      -I${unity-test.dev}/include/unity \
      "''${sources[@]}" "''${testSources[@]}" \
      -L${unity-test}/lib -lunity \
      $(python3-config --ldflags --embed) \
      -o c-tests

    ./c-tests > $out

    runHook postBuild
  '';
}
