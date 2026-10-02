{
  lib,
  stdenvNoCC,
  fetchurl,
  zig,
  python3,
  python3Packages,
  src,
  baseWheel,
  release,
  pythonMinor,
  python,
  platformName,
  platform,
}:

let
  version = (lib.importTOML (src + "/pyproject.toml")).project.version;

  nodot = lib.replaceStrings [ "." ] [ "" ] pythonMinor;
  moduleName = lib.replaceStrings [ "{nodot}" ] [ nodot ] platform.moduleName;

  distribution = fetchurl {
    url =
      "https://github.com/astral-sh/python-build-standalone/releases/download/"
      + "${release}/cpython-${python.version}+${release}-"
      + "${platform.pbsTriple}${python.pbsVariant}-install_only_stripped.tar.gz";
    hash = python.hashes.${platformName};
  };
in
stdenvNoCC.mkDerivation {
  pname = "salix-wheel-${pythonMinor}-${platformName}";
  inherit version src;

  nativeBuildInputs = [
    zig
    python3
    python3Packages.wheel
  ];

  dontConfigure = true;
  dontInstall = true;

  buildPhase = ''
    runHook preBuild

    export ZIG_GLOBAL_CACHE_DIR="$NIX_BUILD_TOP/zig-cache"

    mkdir -p "$NIX_BUILD_TOP/python"
    tar xzf ${distribution} --strip-components=1 -C "$NIX_BUILD_TOP/python"

    mapfile -t cFlags < <(python3 build_config.py c-flags --strict --shipped)
    mapfile -t sources < <(python3 build_config.py sources)

    zig cc \
      -target ${platform.zigTarget} \
      "''${cFlags[@]}" \
      -fPIC -shared \
      -I"$NIX_BUILD_TOP/python/include" \
      -I"$NIX_BUILD_TOP/python/include/python${pythonMinor}" \
      "''${sources[@]}" \
      ${lib.optionalString platform.linkPythonLibrary ''"$NIX_BUILD_TOP/python/libs/python${nodot}.lib"''} \
      ${lib.escapeShellArgs platform.extraFlags} \
      -o "$NIX_BUILD_TOP/${moduleName}"

    wheel unpack --dest "$NIX_BUILD_TOP/unpacked" ${baseWheel}/*.whl
    unpacked=("$NIX_BUILD_TOP"/unpacked/*/)

    rm "''${unpacked[0]}"/salix/__init__.*.so
    cp "$NIX_BUILD_TOP/${moduleName}" "''${unpacked[0]}/salix/${moduleName}"

    mkdir -p "$out"
    wheel pack --dest-dir "$out" "''${unpacked[0]}"
    wheel tags --remove \
      --python-tag ${python.tag} \
      --abi-tag ${python.abiTag} \
      --platform-tag ${platform.platformTag} \
      "$out"/*.whl

    runHook postBuild
  '';

  meta = {
    description = "salix wheel for CPython ${pythonMinor} on ${platformName}";
    homepage = "https://github.com/JPHutchins/salix";
    platforms = lib.platforms.all;
  };
}
