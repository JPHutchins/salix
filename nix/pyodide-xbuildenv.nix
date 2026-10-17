{
  runCommand,
  fetchurl,
  pyodide,
}:

let
  inherit (pyodide.xbuildenv) version hash;
in
runCommand "pyodide-xbuildenv-${version}"
  {
    src = fetchurl {
      url = "https://github.com/pyodide/pyodide/releases/download/${version}/xbuildenv-${version}.tar.gz";
      inherit hash;
    };
  }
  ''
    mkdir -p "$out"
    tar xzf "$src" --strip-components=1 -C "$out"
  ''
