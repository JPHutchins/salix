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
  pname = "salix-sdist";
  version = (lib.importTOML (src + "/pyproject.toml")).project.version;
  inherit src;

  nativeBuildInputs = [ interpreter ];

  dontConfigure = true;
  dontInstall = true;

  buildPhase = ''
    runHook preBuild

    mkdir -p "$out"
    python -m build --sdist --no-isolation --outdir "$out"

    runHook postBuild
  '';
}
