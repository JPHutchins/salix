{
  release = "20260728";

  platforms = {
    manylinux-x86_64 = {
      pbsTriple = "x86_64-unknown-linux-gnu";
      zigTarget = "x86_64-linux-gnu.2.17";
      platformTag = "manylinux_2_17_x86_64";
      moduleName = "__init__.cpython-{nodot}-x86_64-linux-gnu.so";
      extraFlags = [ ];
      linkPythonLibrary = false;
    };
    manylinux-aarch64 = {
      pbsTriple = "aarch64-unknown-linux-gnu";
      zigTarget = "aarch64-linux-gnu.2.17";
      platformTag = "manylinux_2_17_aarch64";
      moduleName = "__init__.cpython-{nodot}-aarch64-linux-gnu.so";
      extraFlags = [ ];
      linkPythonLibrary = false;
    };
    macos-x86_64 = {
      pbsTriple = "x86_64-apple-darwin";
      zigTarget = "x86_64-macos.10.13";
      platformTag = "macosx_10_13_x86_64";
      moduleName = "__init__.cpython-{nodot}-darwin.so";
      extraFlags = [
        "-undefined"
        "dynamic_lookup"
      ];
      linkPythonLibrary = false;
    };
    macos-aarch64 = {
      pbsTriple = "aarch64-apple-darwin";
      zigTarget = "aarch64-macos.11.0";
      platformTag = "macosx_11_0_arm64";
      moduleName = "__init__.cpython-{nodot}-darwin.so";
      extraFlags = [
        "-undefined"
        "dynamic_lookup"
      ];
      linkPythonLibrary = false;
    };
    windows-x86_64 = {
      pbsTriple = "x86_64-pc-windows-msvc";
      zigTarget = "x86_64-windows-gnu";
      platformTag = "win_amd64";
      moduleName = "__init__.pyd";
      extraFlags = [ ];
      linkPythonLibrary = true;
    };
    windows-aarch64 = {
      pbsTriple = "aarch64-pc-windows-msvc";
      zigTarget = "aarch64-windows-gnu";
      platformTag = "win_arm64";
      moduleName = "__init__.pyd";
      extraFlags = [ ];
      linkPythonLibrary = true;
    };
  };

  pythons = {
    "3.14" = {
      version = "3.14.6";
      tag = "cp314";
      abiTag = "cp314";
      pbsVariant = "";
      hashes = {
        manylinux-x86_64 = "sha256-1iVEYtX+BmgQpHLShoYfahj2ZAAAi/qWiOICpWcuiMs=";
        manylinux-aarch64 = "sha256-RPhtWPO7wapsle6Q8DIm4P29IkdamMRBDlogBCt/QNM=";
        macos-x86_64 = "sha256-AKIjY0AqGhXU+xMnyCWakRGCWNVGPRCpfT5WwfGBlfY=";
        macos-aarch64 = "sha256-9LR2WeLaS5fzjO/fWtGfAEKUYJnYQ83mDeMIcI5bGsU=";
        windows-x86_64 = "sha256-GatxPpCNR2r78rZi9yz22Fxd5u1EobKIFez76jeyDVQ=";
        windows-aarch64 = "sha256-gBVuDgUz9g0jrmBrWyaGaq7DEf35iIR2MZZaG+MRg5w=";
      };
    };
    "3.14t" = {
      version = "3.14.6";
      tag = "cp314";
      abiTag = "cp314t";
      pbsVariant = "-freethreaded";
      hashes = {
        manylinux-x86_64 = "sha256-qBH94HTiKlkuAZEvjxsnTTSxpNUXpbU5dWFNV3i8CSQ=";
        manylinux-aarch64 = "sha256-u35HotUc6LLtWymTgOmrEMqum3wO7TvLOrIp271UC/4=";
        macos-x86_64 = "sha256-UjccKbjzkSIcXGwvct0+Ue6VkUHPpbi4aHdB2sqh20U=";
        macos-aarch64 = "sha256-KGnVIdTEhveOosvgMq4Vzz4f9zqKrUs45SC51FJna2Y=";
        windows-x86_64 = "sha256-rnYw2l4m2UgTt7cXuhl6KjaivuUcyjtFl0skHIccmxU=";
      };
    };
    "3.13" = {
      version = "3.13.14";
      tag = "cp313";
      abiTag = "cp313";
      pbsVariant = "";
      hashes = {
        manylinux-x86_64 = "sha256-ZzTD5kPHXoYMNu46eQTo5rr78yMtibF//V+/pyqygWw=";
        manylinux-aarch64 = "sha256-Hq+XmvbGmGVTuRqeOwNkf2POUqiI4AiS073clvQ3SOk=";
        macos-x86_64 = "sha256-qnPDeuvr47cmTc4eSZI3GasKwPxZA1Ot85Pu4+IEHBg=";
        macos-aarch64 = "sha256-qioFT14EveY64Znju2u7Y05FdCPv0pSELe6xKZ5+WTI=";
        windows-x86_64 = "sha256-oJGrkU8rfS28UunPSiJRkPcnCfx5pk7ES8YcpNKQimQ=";
        windows-aarch64 = "sha256-4o5xCKSzbAwyHajIQqet31k1jia17Jq+AC6elAEw9B8=";
      };
    };
    "3.13t" = {
      version = "3.13.14";
      tag = "cp313";
      abiTag = "cp313t";
      pbsVariant = "-freethreaded";
      hashes = {
        manylinux-x86_64 = "sha256-nY/7zldDy2FkFfhphBxxd6o3Re0kjcDrGegD6ORX9G4=";
        manylinux-aarch64 = "sha256-/L69FslIN5GS9FdjiYnAMHYFEUQyAPH+OvPmYEJsC2Y=";
        macos-x86_64 = "sha256-hu3E+B80wXvkFB8a93urIDy3d7DjXArlan0nC0cjX30=";
        macos-aarch64 = "sha256-nLGUnctTxQJguWoN3jJi101kTAcIPlr6zeBTXmI9e0U=";
        windows-x86_64 = "sha256-YLXk/ZuimaUGcjuvI9nmpplckK+7VaIvQn0a3azSAVc=";
      };
    };
    "3.12" = {
      version = "3.12.13";
      tag = "cp312";
      abiTag = "cp312";
      pbsVariant = "";
      hashes = {
        manylinux-x86_64 = "sha256-fOr0Ng9LSrRPMdghfP3HDfjhK2HZVhtJAO56+HF25Q0=";
        manylinux-aarch64 = "sha256-4SXT51A0/nXUmuhXe1N1iwu314F/rd4sAgrSz3r+ZMQ=";
        macos-x86_64 = "sha256-5lTCHQulPixnGGjUES+sWHTeykw1Im02xc/lO8XJzXE=";
        macos-aarch64 = "sha256-LxjN70ElyhRA3RugDryyZ1Ju+1MhOMCGBDj3VepO66w=";
        windows-x86_64 = "sha256-JCuUs3aCrFX5v562JDSNyNF8ZPdPVgKBBFReo//jXiY=";
        windows-aarch64 = "sha256-nw5dDg4F8iWfYKE9C+Baos4BIbcKmHMNRxRRlSaLw3Q=";
      };
    };
    "3.11" = {
      version = "3.11.15";
      tag = "cp311";
      abiTag = "cp311";
      pbsVariant = "";
      hashes = {
        manylinux-x86_64 = "sha256-Od/XlWDZsMIs59FrzMFx7ZNTczGowmqK6+lgMBYhtl4=";
        manylinux-aarch64 = "sha256-pt7NGACZ5naCab2OiWiuqoS7AVEfAPaSHcIE/mSHKao=";
        macos-x86_64 = "sha256-dSuPyYScnFiBenJelbpIj4+Fq348DYqaF5R7gAT3x1I=";
        macos-aarch64 = "sha256-Pxg54GyKCACsMbNdGmMzI92hHTStjy7Z1wzFHFZ5MCg=";
        windows-x86_64 = "sha256-ZKgEERgwxTKb/FpNldbLy7N3yqLAIZUQHt+F0V/FMJk=";
        windows-aarch64 = "sha256-9XQHR4l5b93FuaYPZ0SDNbnTOmrp/bsARcq+M7ZoUCM=";
      };
    };
    "3.10" = {
      version = "3.10.20";
      tag = "cp310";
      abiTag = "cp310";
      pbsVariant = "";
      hashes = {
        manylinux-x86_64 = "sha256-n10LsdDRUPAKJ3dDr1f/hz99caoHCipv3c2rrHOkVHc=";
        manylinux-aarch64 = "sha256-UCJpGbS+PxPu5qBbMLckG0M4Xet9wWc+fFxnv0OBJNQ=";
        macos-x86_64 = "sha256-Sfl/K8pClsHDwtW1MzRyp6UwCeR6HsAlCnLYUpdLQnI=";
        macos-aarch64 = "sha256-4HAoLe8oBiIBAyYFan/cSpWsWIrmlhGg8t/TM9u5G08=";
        windows-x86_64 = "sha256-OZN+eU3UlsAi4TXa8G4w6J+OEk1K4mXqHNrHa+M+XBI=";
      };
    };
    "3.15" = {
      version = "3.15.0b4";
      tag = "cp315";
      abiTag = "cp315";
      pbsVariant = "";
      hashes = {
        manylinux-x86_64 = "sha256-gnN/YnKVdiHwu3h7MqHQEq9GRq3XsEwG5n4M6UAtSZg=";
        manylinux-aarch64 = "sha256-1OsX9ZiWp8bwbJG/xImAGhXWyrmiDvMmKOgD2yNznkA=";
        macos-x86_64 = "sha256-DXoNBOwiyhrActx9yhvmUoWancT9wawSO4ndPVvQG90=";
        macos-aarch64 = "sha256-njKWz6NnrttcClwW4SCiuzncOyPXEPXQ/vlRXdYbpO4=";
        windows-x86_64 = "sha256-MLcp6HiCggm47Xz5J4Z/waDWDWiAYh64gdm2J9C6ojE=";
        windows-aarch64 = "sha256-hYvMVpWh6707FlXSj0j51PsmCDmM1ocqmH8bhJi2bVA=";
      };
    };
    "3.15t" = {
      version = "3.15.0b4";
      tag = "cp315";
      abiTag = "cp315t";
      pbsVariant = "-freethreaded";
      hashes = {
        manylinux-x86_64 = "sha256-qzOhXyD/Ujehp5KIqnAaf918JXv4cmc8q3b4czpLH7k=";
        manylinux-aarch64 = "sha256-61Yj6BK+RCuWKfw8hVPtGWvZ2sjByiqYzkthukfnc7I=";
        macos-x86_64 = "sha256-1+580CrZWtOz3Ovo9AfQP/D6QlxwmoBe8v7WnoZOtMk=";
        macos-aarch64 = "sha256-mRFxpSUWdyPaQhun7zcNrqo+WORf7SUPtR54YC9/79I=";
        windows-x86_64 = "sha256-KRlVdbCsuLhYEg7awI1nDUBx1a+g+5Roa9C2qoTXNps=";
      };
    };
  };
}
