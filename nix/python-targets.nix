{
  release = "20261009";

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
      version = "3.14.8";
      tag = "cp314";
      abiTag = "cp314";
      pbsVariant = "";
      hashes = {
        manylinux-x86_64 = "sha256-3ckC/SZGD3KHWMZ9gEF8EDHt+XWJ8fFixLo6oVijWZg=";
        manylinux-aarch64 = "sha256-VPnNE7l63Wu9B3F1ttifl0zFguzhxArOGJMGdYLtrw4=";
        macos-x86_64 = "sha256-D+DGin+QRYq9E7UKPY9fDLWy8KHzO27xRFdRzy3Is24=";
        macos-aarch64 = "sha256-k2IXwOaJP1Gvk5R0vuGOz15v5HuYbSwWPxm1fGguSoM=";
        windows-x86_64 = "sha256-W2+n3eq+I66D17VOUEPrT6HMsQpz+kxrD1Qd6oFg1QU=";
        windows-aarch64 = "sha256-gu9THZ0jz8DPNtANJ8tGUtrjcKExfAs0t+sZiTtzZuA=";
      };
    };
    "3.14t" = {
      version = "3.14.8";
      tag = "cp314";
      abiTag = "cp314t";
      pbsVariant = "-freethreaded";
      hashes = {
        manylinux-x86_64 = "sha256-+t2OTwgAJc4jqWuqBsdxjYcbddACXySjjwWmU1xTcto=";
        manylinux-aarch64 = "sha256-jurMHz3wrESivrCtZpNOf3/zHMLuBfOmUBHL7p40mj4=";
        macos-x86_64 = "sha256-Jgr0QQBlkLN3PSFVljS0+x1EUhdRuGr0aoiTS92nZnQ=";
        macos-aarch64 = "sha256-Uxo1szW3xfmUhVIcOLDAo8g9XT8NiQReOJLWkNIxlDo=";
        windows-x86_64 = "sha256-Wau8pTzQ0dNKba+El3rXDoh6LhmLzqhvqA3pILrVR/Y=";
      };
    };
    "3.13" = {
      version = "3.13.16";
      tag = "cp313";
      abiTag = "cp313";
      pbsVariant = "";
      hashes = {
        manylinux-x86_64 = "sha256-JIAeoxx9QG1VmhfiImMlxyPVDFaQC5wNp7Yq2q4q/8k=";
        manylinux-aarch64 = "sha256-UsaZOr1qxofM63h+zug7FmJxvAatgKycrILF0o08ALw=";
        macos-x86_64 = "sha256-vQhcUelg8A7jO2MOKxOskP6QzcPaJaDwvAcZsSlJIL8=";
        macos-aarch64 = "sha256-EXhRVdD3DmNBeCGcIJJdjyC+AKPaRoT8T1AZ3RVgLvc=";
        windows-x86_64 = "sha256-Onrftp4/8CpB7TisY3gIUytDirPIYZr0bKXb7FwfUZA=";
        windows-aarch64 = "sha256-mXclP4cCzvaJDymQ3eH+eGrncXtZyo0bsfn1UbwYSvc=";
      };
    };
    "3.13t" = {
      version = "3.13.16";
      tag = "cp313";
      abiTag = "cp313t";
      pbsVariant = "-freethreaded";
      hashes = {
        manylinux-x86_64 = "sha256-5N/MpYxa1iaTlG0tBH1w3tp4nrExIw1AhSx4H7476pc=";
        manylinux-aarch64 = "sha256-mZUBpZBB7cdNg7keDFme0+ipxEmg7dA9yjEl8sSjncU=";
        macos-x86_64 = "sha256-scs3rNwveMiPdVPIPNJHFKE1BTJDc4rjuKwiSiwp0kk=";
        macos-aarch64 = "sha256-KYlpIKNWdQtVlZZ3HlK7anNfZeuVtmBxH25pLssdPEk=";
        windows-x86_64 = "sha256-VjYz+wLCgOO2ZJMqjimVLj7vYEM9cC5wMj9GlqZu4lM=";
      };
    };
    "3.12" = {
      version = "3.12.15";
      tag = "cp312";
      abiTag = "cp312";
      pbsVariant = "";
      hashes = {
        manylinux-x86_64 = "sha256-/6j4XxtW6ItofwjXQ9gXAVUkuGqTZf2SEkMTK4Dxo20=";
        manylinux-aarch64 = "sha256-Lhug5/IuAcU0yuv38J54IDIrwNBVJx00NK9nHoQA+W8=";
        macos-x86_64 = "sha256-+zGH2VM0gTuIlA2w8cpXuEzt/VUnOcOldxw+oOSZiMk=";
        macos-aarch64 = "sha256-Iz+OFSVbPh0f1H6l3VIw3+2APzcROOiW5U2Yxgrhn28=";
        windows-x86_64 = "sha256-Ee2fimrGT7G1pV5A4wWmdEo/d6cIY8j4GXpuQ0mrxko=";
        windows-aarch64 = "sha256-4sHW3mMR2Vt7+vPEZYfLVoLkNBXBn8vvl1lcsLJi9hQ=";
      };
    };
    "3.11" = {
      version = "3.11.17";
      tag = "cp311";
      abiTag = "cp311";
      pbsVariant = "";
      hashes = {
        manylinux-x86_64 = "sha256-idt2B+utt3pkcJ0Vm4ZVXZlTaOw4WiqzOwBHd5DFQ7A=";
        manylinux-aarch64 = "sha256-fn4k7uNGfdZmr/N5w0xO8+a8cUKux9W6eDdl332pmaY=";
        macos-x86_64 = "sha256-rxmwn3nnhLkSrIssZZgFrCbrW4x9x5MVHqTdLjRW8CM=";
        macos-aarch64 = "sha256-9Y04r0aA+/n4nDKecyt8yhvQb/epOIR5iyfVQb7hOPY=";
        windows-x86_64 = "sha256-wwRRFUZbtKp9HKa/5s4xG6FX1ybXaLuzUg+tJYCDHD8=";
        windows-aarch64 = "sha256-+17yPv8PSm/Q1c3hXiXEqbt832Sp+1EUepsBYhhEZlI=";
      };
    };
    "3.10" = {
      version = "3.10.22";
      tag = "cp310";
      abiTag = "cp310";
      pbsVariant = "";
      hashes = {
        manylinux-x86_64 = "sha256-yqZrMI4Q81uBFAVN99knkD+XH2Mc+jI3uExA1dXzVUk=";
        manylinux-aarch64 = "sha256-7L6Xin7L6zqa7We7XRYRgHAVE7T6gQEDeyo/GsEelr4=";
        macos-x86_64 = "sha256-fFecPZkTo2uGpjdycEzAiWDem4GbiRVFUhilNtvU8/I=";
        macos-aarch64 = "sha256-BWlS6O4mbfDcTllaiKDWke/g1q982edyuJ5e/SdurXk=";
        windows-x86_64 = "sha256-8gKIxF/xHn/gJvQN3Psqz+aVzsGIYHRmOeKJFCpYl0U=";
      };
    };
    "3.15" = {
      version = "3.15.0";
      tag = "cp315";
      abiTag = "cp315";
      pbsVariant = "";
      hashes = {
        manylinux-x86_64 = "sha256-Ru3I9vzK3kfcOAAe8tQJTZzt0hoYY9m0SyoCwe52uU0=";
        manylinux-aarch64 = "sha256-ADReR7f0LRDsYNKLLMrWsfM8QGl/CuV+4V1wiW760LU=";
        macos-x86_64 = "sha256-diU+VhlpS2boF6CRu9vz24m7Cv9td6mseq4s6WIY7Q0=";
        macos-aarch64 = "sha256-Rz5iE6HFXaEuYGQiSaYgH+ncqEG0rYxZFJeYhG7YfGI=";
        windows-x86_64 = "sha256-CUOFRjT6D8T68qN9kAvH4ab4mv16xXayLRfzkULGO1U=";
        windows-aarch64 = "sha256-M/thYqT8rte3aDYiY4+LIr99cirLRheT+ZHWUiyXh8s=";
      };
    };
    "3.15t" = {
      version = "3.15.0";
      tag = "cp315";
      abiTag = "cp315t";
      pbsVariant = "-freethreaded";
      hashes = {
        manylinux-x86_64 = "sha256-S/PA6/Rai7b5kxv0w2RLn4PLdewjP2pL5fqpQ8/pKsk=";
        manylinux-aarch64 = "sha256-5FZTk2+PY5yB1WO2tyDR7lRoTW/ozJi5b8i1B3ydmSU=";
        macos-x86_64 = "sha256-x1x8aaiEsHIgKK3hSr+mCXl08rPJfMGf+yJij0d61mE=";
        macos-aarch64 = "sha256-+qLJcGt67aV5kwyH8YhsSQT4Rx3T9gNCvhKD37tQKB8=";
        windows-x86_64 = "sha256-u73fBlMH3vH39/MVJcGnNcGwFUF7iT8uL/h2LX1omXM=";
      };
    };
  };
}
