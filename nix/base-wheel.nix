{
  lib,
  stdenv,
  python3,
  src,
}:

let
  interpreter = python3.withPackages (packages: [
    packages.build
    packages.setuptools
    packages.wheel
  ]);
in
stdenv.mkDerivation {
  pname = "salix-base-wheel";
  version = (lib.importTOML (src + "/pyproject.toml")).project.version;
  inherit src;

  nativeBuildInputs = [ interpreter ];

  dontConfigure = true;
  dontInstall = true;

  SALIX_STRICT = "1";

  buildPhase = ''
    runHook preBuild

    mkdir -p "$out"
    python -m build --wheel --no-isolation --outdir "$out"

    runHook postBuild
  '';
}
