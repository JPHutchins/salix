{
  lib,
  rustPlatform,
  fetchCrate,
}:

rustPlatform.buildRustPackage rec {
  pname = "jphfmt";
  version = "0.2.2";

  src = fetchCrate {
    inherit pname version;
    hash = "sha256-NLpCZfhM5oVrg+jVTbGQ/zjBT+U9bZeVi73ENGyNVDU=";
  };

  cargoLock.lockFile = ./jphfmt-Cargo.lock;

  meta = {
    description = "Zero-configuration C formatter";
    homepage = "https://github.com/JPHutchins/jphfmt";
    mainProgram = "jphfmt";
  };
}
