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
    set -o pipefail

    source ${./read-build-config.sh}
    readBuildConfig cFlags c-flags --strict
    readBuildConfig sources sources
    readBuildConfig testSources test-sources

    $CC -DTESTING \
      "''${cFlags[@]}" \
      $(python3-config --includes) \
      -I${unity-test.dev}/include/unity \
      "''${sources[@]}" "''${testSources[@]}" \
      -L${unity-test}/lib -lunity \
      $(python3-config --ldflags --embed) \
      -o c-tests

    ./c-tests | tee $out

    runHook postBuild
  '';
}
