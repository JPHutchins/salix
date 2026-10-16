readBuildConfig() {
  python3 build_config.py "${@:2}" > "$NIX_BUILD_TOP/$1"
  mapfile -t "$1" < "$NIX_BUILD_TOP/$1"
}
