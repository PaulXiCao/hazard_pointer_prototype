{
  description = "C++26 std::hazard_pointer prototype (P2530R3)";

  # Stable channel, not unstable -- a prototype whose whole purpose is
  # reproducing memory-ordering behaviour should not have its compiler move
  # under it.  Only nixpkgs as an input; eachDefaultSystem is a four-line
  # genAttrs here, not worth a flake-utils dependency.
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" "x86_64-darwin" "aarch64-darwin" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (s: f nixpkgs.legacyPackages.${s});
    in
    {
      # herdtools7 is not in nixpkgs; tools/litmus/herdtools7.nix pins 7.58, the
      # version used for the POWER hardware runs in the review.  Exposed as a
      # package so CI can use the same build the dev shell does, instead of an
      # opam switch that can drift.
      packages = forAllSystems (pkgs: {
        herdtools7 = import ./tools/litmus/herdtools7.nix { inherit pkgs; };
      });

      devShells = forAllSystems (pkgs: {
        default = pkgs.mkShell {
          name = "hazard-pointer-prototype";

          packages = with pkgs; [
            clang_21                      # asan/tsan presets; matches CI's clang-21
            llvmPackages_21.clang-tools   # clang-tidy, clang-format
            llvm_21                       # llvm-symbolizer -- readable sanitizer traces
            gcc14                         # the other half of the CI compiler matrix
            cmake
            ninja
            git                           # CPM shells out to git
            util-linux                    # setarch -- see sanitizer note below
            self.packages.${pkgs.stdenv.hostPlatform.system}.herdtools7
          ];

          shellHook = ''
            # Must be exported here, not as mkShell attributes: stdenv's setup
            # exports CC/CXX for its own compiler afterwards and would win.
            # CI's sanitizer job runs clang, so default to it.  For the gcc half
            # of the matrix: CXX=g++ CC=gcc cmake --preset dev ...
            export CXX=clang++
            export CC=clang
            export ASAN_SYMBOLIZER_PATH=${pkgs.llvm_21}/bin/llvm-symbolizer
            cat <<'EOF'
hazard_pointer prototype dev shell

  cmake --preset dev  && cmake --build --preset dev  && ctest --preset dev
  bash tools/litmus/run.sh          # herd7 memory-model check (no opam needed)

Sanitizers need ASLR turned down -- this kernel randomizes more address bits
than TSan/ASan can cope with, and every sanitized binary dies immediately with
"FATAL: ThreadSanitizer: unexpected memory mapping".  Prefix with setarch:

  cmake --preset tsan && cmake --build --preset tsan
  setarch $(uname -m) -R ctest --preset tsan

  cmake --preset asan && cmake --build --preset asan
  setarch $(uname -m) -R ctest --preset asan

(The alternative is system-wide: boot.kernel.sysctl."vm.mmap_rnd_bits" = 28.
setarch needs no root, so prefer it.)

First configure downloads CPM + GTest, so it needs network.
EOF
          '';
        };
      });
    };
}
