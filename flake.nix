{
  description = "A flake for building msxiv";

  inputs = {
    # nixpkgs.url = "github:NixOS/nixpkgs";
    nixpkgs.url = "github:NixOS/nixpkgs/e2605d0744c2417b09f8bf850dfca42fcf537d34";
  };

  outputs = { self, nixpkgs }:
  let
    system = "x86_64-linux";
    pkgs = import nixpkgs { inherit system; };
  in
  {
    packages.x86_64-linux.default = pkgs.stdenv.mkDerivation {
      pname = "msxiv";
      version = "1.0.0";
      src = ./.;

      nativeBuildInputs = [
        pkgs.cmake
        pkgs.pkgconf
        pkgs.imagemagick
        pkgs.xorg.libX11
      ];

      buildPhase = ''
        mkdir -p build
        cd build
        cmake .. -DCMAKE_INSTALL_PREFIX=$out
        make
      '';

      installPhase = ''
        cd build
        make install
      '';
    };
  };
}

