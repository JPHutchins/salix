{
  description = "salix — a C-backed, inheritable Struct base class for Python";

  inputs.nixpkgs.url = "github:nixos/nixpkgs/nixos-unstable";

  outputs =
    { self, nixpkgs }:
    let
      inherit (nixpkgs) lib;

      systems = [
        "x86_64-linux"
        "aarch64-linux"
        "aarch64-darwin"
      ];
      forAllSystems = lib.genAttrs systems;

      matrix = import ./nix/python-targets.nix;

      declaredPythons = lib.sort (a: b: a < b) (
        lib.splitString "\n" (lib.removeSuffix "\n" (lib.fileContents ./.python-version + "\n"))
      );
      pinnedPythons = lib.sort (a: b: a < b) (builtins.attrNames matrix.pythons);

      pinnedBaseline = lib.sort (a: b: a < b) (
        lib.unique (map (name: lib.removeSuffix "t" name) pinnedPythons)
      );

      wheelIds = lib.concatMap (
        pythonMinor:
        map (platformName: { inherit pythonMinor platformName; }) (
          builtins.attrNames matrix.pythons.${pythonMinor}.hashes
        )
      ) pinnedPythons;

      abiIsFrozen = version: builtins.match "[0-9]+\\.[0-9]+\\.[0-9]+(rc[0-9]+)?" version != null;

      releasableWheelNames = map ({ pythonMinor, platformName }: "${pythonMinor}-${platformName}") (
        lib.filter ({ pythonMinor, ... }: abiIsFrozen matrix.pythons.${pythonMinor}.version) wheelIds
      );

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

          named = lib.mapAttrs' (
            name: wheel: lib.nameValuePair "wheel-${lib.replaceStrings [ "." ] [ "" ] name}" wheel
          ) wheels;

          jphfmt = pkgs.callPackage ./nix/jphfmt.nix { };

          cTests = pkgs.callPackage ./nix/c-tests.nix { src = testSource; };

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

      devShells = forAllSystems (
        system:
        let
          inherit (forSystem.${system}) pkgs jphfmt;
        in
        {
          default = pkgs.mkShell {
            packages = [
              pkgs.uv
              pkgs.zig
              pkgs.nixfmt
              pkgs.clang-tools
              pkgs.gdb
              pkgs.git
              jphfmt
            ];

            shellHook = ''
              unset PYTHONPATH

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

          nixfmt = pkgs.runCommand "nixfmt-check" { nativeBuildInputs = [ pkgs.nixfmt ]; } ''
            nixfmt --check ${./flake.nix} ${./nix}/*.nix
            touch $out
          '';

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
