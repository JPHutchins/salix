{
  description = "salix — a C-backed, inheritable Struct base class for Python";

  inputs.nixpkgs.url = "github:nixos/nixpkgs/nixos-unstable";

  outputs =
    { self, nixpkgs }:
    let
      inherit (nixpkgs) lib;

      # Which machines can run this flake, which is not the same question as
      # which wheels it builds -- macos-x86_64 is a zig cross target and is
      # unaffected. nixpkgs 26.11 dropped x86_64-darwin, so claiming it here is
      # an evaluation error rather than a capability.
      systems = [
        "x86_64-linux"
        "aarch64-linux"
        "aarch64-darwin"
      ];
      forAllSystems = lib.genAttrs systems;

      matrix = import ./nix/python-targets.nix;

      # .python-version is the single source of truth for the interpreter set;
      # drift between it and the generated pins is a build error, not a
      # silently smaller wheel set.
      declaredPythons = lib.sort (a: b: a < b) (
        lib.splitString "\n" (lib.removeSuffix "\n" (lib.fileContents ./.python-version + "\n"))
      );
      pinnedPythons = lib.sort (a: b: a < b) (builtins.attrNames matrix.pythons);

      # A free-threaded target is a variant of a declared interpreter, not a
      # separate one, so what has to agree with .python-version is the pinned
      # set with the variants folded back in. Which minors get one is the
      # release's answer, not something to restate here.
      pinnedBaseline = lib.sort (a: b: a < b) (
        lib.unique (map (name: lib.removeSuffix "t" name) pinnedPythons)
      );

      wheelIds = lib.concatMap (
        pythonMinor:
        map (platformName: { inherit pythonMinor platformName; }) (
          builtins.attrNames matrix.pythons.${pythonMinor}.hashes
        )
      ) pinnedPythons;

      # CPython freezes the ABI at the first release candidate, and salix reads
      # PyHeapTypeObject's layout directly, so a cp3XX wheel built against an
      # alpha or a beta is a promise it cannot keep -- and PyPI does not take an
      # upload back.
      # Matched against the shapes whose ABI *is* frozen -- a final release, or
      # an rc, which is where CPython freezes it -- so anything unrecognised is
      # held back rather than published. The other direction fails open, and an
      # upload to PyPI is not something a later commit can undo.
      abiIsFrozen = version: builtins.match "[0-9]+\\.[0-9]+\\.[0-9]+(rc[0-9]+)?" version != null;

      releasableWheelNames = map ({ pythonMinor, platformName }: "${pythonMinor}-${platformName}") (
        lib.filter ({ pythonMinor, ... }: abiIsFrozen matrix.pythons.${pythonMinor}.version) wheelIds
      );

      # Only what a wheel is built from, so editing the tests does not
      # invalidate every cross build.
      buildSourceFiles = lib.fileset.unions [
        ./src
        ./salix
        ./build_config.py
        ./setup.py
        ./pyproject.toml
        ./README.md
        ./LICENSE
      ];
      buildSource = lib.fileset.toSource {
        root = ./.;
        fileset = buildSourceFiles;
      };

      # The in-file tests read src/, tests/c/ and the version in pyproject.toml,
      # so touching them does not invalidate a single cross build.
      testSource = lib.fileset.toSource {
        root = ./.;
        fileset = lib.fileset.unions [
          ./src
          ./tests/c
          ./build_config.py
          ./pyproject.toml
        ];
      };

      perSystem =
        system:
        let
          pkgs = nixpkgs.legacyPackages.${system};

          baseWheel = pkgs.callPackage ./nix/base-wheel.nix { src = buildSource; };

          wheels = lib.listToAttrs (
            map (
              { pythonMinor, platformName }:
              lib.nameValuePair "${pythonMinor}-${platformName}" (
                pkgs.callPackage ./nix/wheel.nix {
                  src = buildSource;
                  inherit baseWheel;
                  inherit (matrix) release;
                  inherit pythonMinor platformName;
                  python = matrix.pythons.${pythonMinor};
                  platform = matrix.platforms.${platformName};
                }
              )
            ) wheelIds
          );

          all = pkgs.symlinkJoin {
            name = "salix-wheels";
            paths = lib.attrValues wheels;
          };

          # Nix splits an attribute path on `.`, so `wheel-3.14-*` reaches the
          # parser as `wheel-3` and cannot be built by name.
          named = lib.mapAttrs' (
            name: wheel: lib.nameValuePair "wheel-${lib.replaceStrings [ "." ] [ "" ] name}" wheel
          ) wheels;

          jphfmt = pkgs.callPackage ./nix/jphfmt.nix { };

          cTests = pkgs.callPackage ./nix/c-tests.nix { src = testSource; };

          # MANIFEST.in and the governance docs ride the sdist alone: without
          # the manifest the sdist has no build_config.py and no headers, and a
          # wheel build would only parse it and warn on the misses.
          sdist = pkgs.callPackage ./nix/sdist.nix {
            src = lib.fileset.toSource {
              root = ./.;
              fileset = lib.fileset.unions [
                buildSourceFiles
                ./MANIFEST.in
                ./CODE_OF_CONDUCT.md
              ];
            };
          };

          # What a release uploads: every releasable wheel and the one sdist, in
          # the shape `twine upload` wants. A pre-release interpreter's wheels
          # are still built and still tested, just never published.
          # An sdist on its own is not a release: it would make every user on
          # every platform compile. If the whole matrix is pre-release, say so
          # here rather than in the workflow, where it surfaces as a glob that
          # matched no .whl.
          release =
            assert lib.assertMsg (
              releasableWheelNames != [ ]
            ) "no releasable wheels: every pinned interpreter is a pre-release, so nothing has a frozen ABI";
            pkgs.symlinkJoin {
              name = "salix-release";
              paths = map (name: wheels.${name}) releasableWheelNames ++ [ sdist ];
            };
        in
        {
          inherit
            pkgs
            wheels
            all
            named
            jphfmt
            cTests
            sdist
            release
            ;
        };

      forSystem = forAllSystems perSystem;
    in
    assert lib.assertMsg (declaredPythons == pinnedBaseline) (
      ".python-version lists ${toString declaredPythons} but nix/python-targets.nix pins "
      + "${toString pinnedPythons}; regenerate with tools/update_python_targets.py"
    );
    {
      packages = forAllSystems (
        system:
        forSystem.${system}.named
        // {
          default = forSystem.${system}.all;
          c-tests = forSystem.${system}.cTests;
          inherit (forSystem.${system}) sdist release;
        }
      );

      # The only supported environment: enter it once, then run camas (and any
      # editor or agent) from inside. Every tool the tasks invoke is here, so a
      # task command is bare -- nothing pays `nix develop` per invocation.
      devShells = forAllSystems (
        system:
        let
          inherit (forSystem.${system}) pkgs jphfmt;
        in
        {
          default = pkgs.mkShell {
            # No python here on purpose: uv owns the interpreters, driven by
            # .python-version, which is the single source of truth for which
            # versions this project builds and tests against.
            packages = [
              pkgs.uv
              pkgs.zig
              pkgs.nixfmt
              pkgs.clang-tools
              pkgs.gdb
              pkgs.git
              jphfmt
            ];

            # stdenv exports its own CC during setup, after `env` is applied,
            # so the compiler choice has to be made here to survive.
            shellHook = ''
              unset PYTHONPATH

              # The wheels are cross-compiled with zig, so the local build uses
              # it too -- one compiler, one warning set, no -Werror surprise
              # that only shows up at release time.
              export CC="zig cc"
              export LDSHARED="zig cc -shared"
            '';
          };
        }
      );

      checks = forAllSystems (
        system:
        let
          inherit (forSystem.${system})
            pkgs
            all
            named
            cTests
            sdist
            ;
        in
        named
        // {
          c-tests = cTests;

          # The directory rather than a list of files: three of the six were
          # missing from the list, and a list is a second place to remember.
          nixfmt = pkgs.runCommand "nixfmt-check" { nativeBuildInputs = [ pkgs.nixfmt ]; } ''
            nixfmt --check ${./flake.nix} ${./nix}/*.nix
            touch $out
          '';

          # Five of the six targets cannot run here, so every wheel is checked
          # for internal consistency and for a payload whose architecture and
          # container match the tag it is published under. twine is the gate
          # PyPI itself applies; nothing off the shelf checks the payload.
          wheels-verified =
            pkgs.runCommand "wheels-verified"
              {
                nativeBuildInputs = [
                  pkgs.python314
                  pkgs.python3Packages.twine
                ];
              }
              ''
                python ${./tools/check_wheel.py} ${all}/*.whl
                twine check --strict ${all}/*.whl ${sdist}/*.tar.gz
                touch $out
              '';
        }
        # The native wheel is the only one this builder can execute, so it is
        # the only one whose importability can actually be demonstrated.
        // lib.optionalAttrs (system == "x86_64-linux") {
          wheel-smoke =
            pkgs.runCommand "wheel-smoke"
              {
                nativeBuildInputs = [
                  (pkgs.python314.withPackages (ps: [
                    ps.pip
                    ps.pytest
                    ps.hypothesis
                  ]))
                ];
              }
              ''
                pip install --no-index --no-deps --target=site \
                  ${forSystem.${system}.wheels."3.14-manylinux-x86_64"}/*.whl
                export PYTHONPATH=$PWD/site
                export SALIX_REQUIRE_INSTALLED=1
                python -m pytest -q -p no:cacheprovider ${./tests}
                touch $out
              '';
        }
      );

      formatter = forAllSystems (system: forSystem.${system}.pkgs.nixfmt);
    };
}
