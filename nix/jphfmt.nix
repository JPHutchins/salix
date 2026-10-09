{
  lib,
  rustPlatform,
  fetchCrate,
}:

rustPlatform.buildRustPackage rec {
  pname = "jphfmt";
  version = "0.3.0";

  src = fetchCrate {
    inherit pname version;
    hash = "sha256-qBpcX73EXZ4EweEqQ0xAHPrMTqZBf2777Pkk2ioo9CE=";
  };

  cargoLock.lockFile = ./jphfmt-Cargo.lock;

  meta = {
    description = "Zero-configuration C formatter";
    homepage = "https://github.com/JPHutchins/jphfmt";
    mainProgram = "jphfmt";
  };
}
