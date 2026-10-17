{
  lib,
  stdenvNoCC,
  nodejs,
  python3,
  python3Packages,
  emscripten,
  xbuildenv,
  pyodide,
  src,
  baseWheel,
}:

let
  version = (lib.importTOML (src + "/pyproject.toml")).project.version;

  pythonMinor = lib.versions.majorMinor pyodide.python;
  nodot = lib.replaceStrings [ "." ] [ "" ] pythonMinor;
  moduleName = "__init__.cpython-${nodot}-wasm32-emscripten.so";
in
stdenvNoCC.mkDerivation {
  pname = "salix-wheel-${pythonMinor}-pyodide";
  inherit version src;

  nativeBuildInputs = [
    python3
    python3Packages.wheel
  ];

  dontConfigure = true;
  dontInstall = true;

  buildPhase = ''
    runHook preBuild

    export EM_CONFIG="$NIX_BUILD_TOP/emscripten-config"
    export EM_CACHE="$NIX_BUILD_TOP/emscripten-cache"
    printf "LLVM_ROOT = '%s'\nBINARYEN_ROOT = '%s'\nNODE_JS = '%s'\n" \
      ${emscripten}/bin ${emscripten} ${lib.getExe nodejs} > "$EM_CONFIG"

    source ${./read-build-config.sh}
    readBuildConfig cFlags c-flags --strict --shipped
    readBuildConfig sources sources

    ${emscripten}/emscripten/emcc \
      "''${cFlags[@]}" \
      -sSIDE_MODULE=1 \
      -I${xbuildenv}/pyodide-root/cpython/installs/python-${pyodide.python}/include/python${pythonMinor} \
      "''${sources[@]}" \
      -o "$NIX_BUILD_TOP/${moduleName}"

    wheel unpack --dest "$NIX_BUILD_TOP/unpacked" ${baseWheel}/*.whl
    unpacked=("$NIX_BUILD_TOP"/unpacked/*/)

    rm "''${unpacked[0]}"/salix/__init__.*.so
    cp "$NIX_BUILD_TOP/${moduleName}" "''${unpacked[0]}/salix/${moduleName}"

    mkdir -p "$out"
    wheel pack --dest-dir "$out" "''${unpacked[0]}"
    wheel tags --remove \
      --python-tag cp${nodot} \
      --abi-tag cp${nodot} \
      --platform-tag ${pyodide.platformTag} \
      "$out"/*.whl

    runHook postBuild
  '';

  meta = {
    description = "salix wheel for CPython ${pythonMinor} on Pyodide ${pyodide.xbuildenv.version}";
    homepage = "https://github.com/JPHutchins/salix";
    platforms = [ "x86_64-linux" ];
  };
}
