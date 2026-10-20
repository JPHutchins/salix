{
  lib,
  stdenv,
  python3,
  src,
}:

stdenv.mkDerivation {
  pname = "salix-source-build-glibc";
  version = (lib.importTOML (src + "/pyproject.toml")).project.version;
  inherit src;

  nativeBuildInputs = [ (python3.withPackages (packages: [ packages.setuptools ])) ];

  SALIX_STRICT = "1";

  dontConfigure = true;
  dontInstall = true;

  buildPhase = ''
    runHook preBuild

    python setup.py build_ext --inplace
    readelf --version-info salix/__init__*.so | grep -oE 'GLIBC_[A-Z0-9_.]+' | sort -u | tee $out
    if grep -q '^GLIBC_ABI_' $out; then
      echo "a source build requires $(grep '^GLIBC_ABI_' $out | tr '\n' ' ')which an older glibc lacks" >&2
      exit 1
    fi

    runHook postBuild
  '';

  meta.platforms = [ "x86_64-linux" ];
}
