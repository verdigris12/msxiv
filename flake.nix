{
  description = "A flake for building msxiv";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/e2605d0744c2417b09f8bf850dfca42fcf537d34";
  };

  outputs = { self, nixpkgs }:
  let
    system = "x86_64-linux";
    pkgs = import nixpkgs { inherit system; };
  in
  {
    packages.${system} = {
      msxiv = pkgs.stdenv.mkDerivation {
        pname = "msxiv";
        version = "0.1.1";
        src = ./.;

        nativeBuildInputs = [
          pkgs.cmake
          pkgs.pkg-config
        ];

        buildInputs = [
          pkgs.imagemagick
          pkgs.xorg.libX11
          pkgs.xorg.libXft
          pkgs.xorg.libXext
        ];

      };
    };
  };
}

