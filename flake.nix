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

      matrix = lib.importJSON ./nix/python-targets.json;
      pyodide = import ./nix/pyodide.nix;

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

      untestedPythons = lib.importJSON ./nix/untested-pythons.json;

      testRequirements = (lib.importTOML ./tests/pyproject.toml).project.dependencies;
      testPackages = requirementPackages testRequirements;
      requirementPackages =
        requirements: packages:
        map
          (
            { requirement, name, ... }:
            assert lib.assertMsg (packages ? ${name}) (
              "tests/pyproject.toml requires '${requirement}', but nixpkgs' python${packages.python.pythonVersion} "
              + "package set has no attribute '${name}'"
            );
            packages.${name}
          )
          (
            lib.filter (
              { pythonBelow, ... }:
              pythonBelow == null || lib.versionOlder packages.python.pythonVersion pythonBelow
            ) (map parseRequirement requirements)
          );
      parseRequirement =
        requirement:
        let
          clauses = lib.splitString ";" requirement;
          name = builtins.match " *([A-Za-z0-9][A-Za-z0-9._-]*)( *[<>=!~(][^[@]*)? *" (builtins.head clauses);
          bounds = map (builtins.match " *python_version *< *['\"]([0-9]+[.][0-9]+)['\"] *") (
            builtins.tail clauses
          );
        in
        assert lib.assertMsg (name != null && builtins.length bounds <= 1 && !(lib.elem null bounds)) (
          "tests/pyproject.toml requires '${requirement}', but flake.nix maps only a name, an optional "
          + "version specifier and an optional python_version < 'X.Y' marker to a nixpkgs package"
        );
        {
          inherit requirement;
          name = builtins.head name;
          pythonBelow = if bounds == [ ] then null else builtins.head (builtins.head bounds);
        };

      releasableWheelNames = map ({ pythonMinor, platformName }: "${pythonMinor}-${platformName}") (
        lib.filter (
          { pythonMinor, ... }:
          abiIsFrozen matrix.pythons.${pythonMinor}.version && !(lib.elem pythonMinor untestedPythons)
        ) wheelIds
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

          wheels =
            lib.listToAttrs (
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
            )
            // pyodideWheels;

          pyodideWheels = lib.optionalAttrs (system == "x86_64-linux") {
            pyodide = pkgs.callPackage ./nix/pyodide-wheel.nix {
              src = buildSource;
              inherit baseWheel pyodide emscripten;
              xbuildenv = pyodideXbuildenv;
            };
          };

          all = pkgs.symlinkJoin {
            name = "salix-wheels";
            paths = lib.attrValues wheels;
          };

          named = lib.mapAttrs' (
            name: wheel: lib.nameValuePair "wheel-${lib.replaceStrings [ "." ] [ "" ] name}" wheel
          ) wheels;

          pyodideXbuildenv = pkgs.callPackage ./nix/pyodide-xbuildenv.nix { inherit pyodide; };

          emscripten = pkgs.callPackage ./nix/emscripten.nix { inherit pyodide; };

          pyodidePython = pkgs.callPackage ./nix/pyodide-python.nix { xbuildenv = pyodideXbuildenv; };

          jphfmt = pkgs.callPackage ./nix/jphfmt.nix { };

          cTests = pkgs.callPackage ./nix/c-tests.nix { src = testSource; };

          memcheck = pkgs.callPackage ./nix/memcheck.nix {
            inherit testPackages;
            src = lib.fileset.toSource {
              root = ./.;
              fileset = lib.fileset.unions [
                buildSourceFiles
                ./tests
              ];
            };
          };

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
            assert lib.assertMsg (releasableWheelNames != [ ])
              "no releasable wheels: every pinned interpreter is a pre-release or listed in nix/untested-pythons.json";
            pkgs.symlinkJoin {
              name = "salix-release";
              paths =
                map (name: wheels.${name}) releasableWheelNames ++ lib.attrValues pyodideWheels ++ [ sdist ];
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
            memcheck
            sdist
            release
            pyodidePython
            ;
        };

      forSystem = forAllSystems perSystem;
    in
    assert lib.assertMsg (declaredPythons == pinnedBaseline) (
      ".python-version lists ${toString declaredPythons} but nix/python-targets.json pins "
      + "${toString pinnedPythons}; regenerate with tools/update_python_targets.py"
    );
    assert lib.assertMsg
      (
        builtins.isList untestedPythons
        && lib.all (
          python: builtins.isString python && lib.hasSuffix "t" python && matrix.pythons ? ${python}
        ) untestedPythons
      )
      (
        "nix/untested-pythons.json is ${builtins.toJSON untestedPythons}, but it must be a list of "
        + "free-threaded interpreters nix/python-targets.json pins: ${toString pinnedPythons}"
      );
    {
      packages = forAllSystems (
        system:
        forSystem.${system}.named
        // {
          default = forSystem.${system}.all;
          c-tests = forSystem.${system}.cTests;
          pyodide-python = forSystem.${system}.pyodidePython;
          inherit (forSystem.${system}) sdist release;
        }
        // lib.optionalAttrs (lib.hasSuffix "-linux" system) {
          inherit (forSystem.${system}) memcheck;
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

          test-requirements =
            let
              keptOn =
                pythonVersion:
                requirementPackages [ "plain>=1.0-1" "marked (>= 2); python_version < '3.11'" ] {
                  python = { inherit pythonVersion; };
                  plain = "plain";
                  marked = "marked";
                };
            in
            assert lib.assertMsg
              (
                keptOn "3.10" == [
                  "plain"
                  "marked"
                ]
                && keptOn "3.11" == [ "plain" ]
              )
              (
                "requirementPackages keeps ${builtins.toJSON (keptOn "3.10")} on 3.10 and "
                + "${builtins.toJSON (keptOn "3.11")} on 3.11, not the marked requirement on 3.10 only"
              );
            builtins.deepSeq (map parseRequirement testRequirements) pkgs.emptyFile;

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
                  (pkgs.python314.withPackages (ps: [ ps.pip ] ++ testPackages ps))
                ];
              }
              ''
                pip install --no-index --no-deps --target=site \
                  ${forSystem.${system}.wheels."3.14-manylinux-x86_64"}/*.whl
                export PYTHONPATH=$PWD/site
                export SALIX_REQUIRE_INSTALLED=1
                cp ${./README.md} README.md
                python -m pytest -q -p no:cacheprovider ${./tests}
                python -m pytest -q -p no:cacheprovider --markdown-docs README.md
                touch $out
              '';
        }
      );

      formatter = forAllSystems (system: forSystem.${system}.pkgs.nixfmt);
    };
}
