# jphfmt is not in nixpkgs, and the dev shell is the only supported
# environment, so the formatter is pinned here rather than left to a
# `cargo install` the shell cannot guarantee.
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

  # cargoHash would route the vendor step through nixpkgs' Python fetcher,
  # which crates.io currently answers with 403; the lockfile path fetches each
  # crate with fetchurl instead. The lock is copied verbatim from the crate.
  cargoLock.lockFile = ./jphfmt-Cargo.lock;

  meta = {
    description = "Zero-configuration C formatter";
    homepage = "https://github.com/JPHutchins/jphfmt";
    mainProgram = "jphfmt";
  };
}
