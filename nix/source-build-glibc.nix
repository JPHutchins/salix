{
  lib,
  stdenv,
  python3,
  src,
  glibcFloor,
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
    module=(salix/__init__*.so)

    if readelf --relocs "''${module[@]}" | grep -q R_X86_64_TLSDESC; then
      echo "a source build reaches its thread-locals through TLS descriptors (the gnu2 dialect)" >&2
      exit 1
    fi

    readelf --version-info "''${module[@]}" | grep -oE 'GLIBC_[A-Z0-9_.]+' | sort -u | tee $out
    floor=GLIBC_${glibcFloor}
    absent_from_floor=$(
      while read -r needed; do
        [[ $needed =~ ^GLIBC_2\.[0-9.]+$ && $(printf '%s\n' "$needed" "$floor" | sort -V | tail -1) == "$floor" ]] \
          || echo "$needed"
      done < $out
    )
    if [ -n "$absent_from_floor" ]; then
      echo "a source build requires" $absent_from_floor "which $floor, the wheels' floor, lacks" >&2
      exit 1
    fi

    runHook postBuild
  '';

  meta.platforms = [ "x86_64-linux" ];
}
