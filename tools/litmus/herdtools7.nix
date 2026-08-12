# herdtools7 is not in nixpkgs, and the CI job installs it via opam.  This
# derivation exists so the same version can be run locally on NixOS without an
# opam switch:
#
#   nix-shell tools/litmus/herdtools7.nix --run 'bash tools/litmus/run.sh'
#
# 7.58 matches the version Thomas Rodgers used for the hardware litmus7 runs.
{ pkgs ? import <nixpkgs> {} }:

pkgs.ocamlPackages.buildDunePackage rec {
  pname = "herdtools7";
  version = "7.58";

  src = pkgs.fetchzip {
    url = "https://github.com/herd/herdtools7/archive/refs/tags/${version}.tar.gz";
    sha256 = "16iwlmp806svip07qmz8ibj768ps08z7nyn31bk2z3hgw7775syk";
  };

  nativeBuildInputs = [ pkgs.ocamlPackages.menhir ];
  buildInputs = [ pkgs.ocamlPackages.menhirLib pkgs.ocamlPackages.zarith ];

  # Version.ml is generated, not shipped -- upstream's Makefile runs
  # version-gen.sh before dune.  It also bakes in the libdir where herd7 looks
  # for its .cat models, so $out must be passed here and not fixed up later.
  preBuild = ''
    sh ./version-gen.sh $out
  '';

  # `dune install` ships only the binaries; upstream's dune-install.sh also
  # copies the .cat/.bell model files into share/herdtools7.  Without these,
  # herd7 builds fine and then fails at run time with "cannot find rc11.cat".
  postInstall = ''
    for d in herd litmus jingle; do
      mkdir -p $out/share/herdtools7/$d
      cp -r $d/libdir/. $out/share/herdtools7/$d/
    done
  '';

  dontDetectOcamlConflicts = true;

  meta = with pkgs.lib; {
    description = "The herdtools7 suite (herd7, litmus7, diy7)";
    homepage = "https://github.com/herd/herdtools7";
    license = licenses.cecill-b;
  };
}
