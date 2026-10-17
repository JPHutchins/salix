{
  writeShellApplication,
  nodejs,
  coreutils,
  which,
  xbuildenv,
}:

writeShellApplication {
  name = "pyodide-python";
  runtimeInputs = [
    nodejs
    coreutils
    which
  ];
  text = ''
    exec ${xbuildenv}/pyodide-root/dist/python "$@"
  '';
}
