{
  stdenv,
  fetchurl,
  autoPatchelfHook,
  zlib,
  pyodide,
}:

stdenv.mkDerivation {
  pname = "emscripten";
  inherit (pyodide.emscripten) version;

  src = fetchurl {
    url =
      "https://storage.googleapis.com/webassembly/emscripten-releases-builds/linux/"
      + "${pyodide.emscripten.release}/wasm-binaries.tar.xz";
    inherit (pyodide.emscripten) hash;
  };

  sourceRoot = "install";

  nativeBuildInputs = [ autoPatchelfHook ];
  buildInputs = [
    zlib
    stdenv.cc.cc.lib
  ];

  dontConfigure = true;
  dontBuild = true;

  installPhase = ''
    runHook preInstall

    rm -r emscripten/{cache,node_modules,test}
    mkdir -p "$out/bin"
    mv bin/clang bin/clang-23 bin/llvm-ar bin/llvm-objcopy bin/lld bin/wasm-ld bin/wasm-opt "$out/bin"
    mv lib emscripten "$out"

    runHook postInstall
  '';

  meta = {
    description = "Emscripten ${pyodide.emscripten.version}, the compiler Pyodide ${pyodide.xbuildenv.version} is built with";
    homepage = "https://emscripten.org";
    platforms = [ "x86_64-linux" ];
  };
}
