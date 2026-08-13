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
        # Just enough to configure and build -- the contracts CI job's shell.
        # Kept separate from `default` so the everyday shell does not carry
        # gcc16's closure on top of the clang/gcc14/herdtools7 it already has.
        #
        # gcc-16 used to come from ppa:ubuntu-toolchain-r/test, but
        # add-apt-repository reads from Launchpad on every run and died there
        # (IncompleteRead) on 2026-08-12.  Caching the .debs would not have
        # helped: GitHub evicts caches untouched for 7 days and this repo's runs
        # are weeks apart, so the PPA would still be hit nearly every time.
        # nixpkgs ships gcc 16.1.0 prebuilt (92 MB from cache.nixos.org, no
        # source build) and pins it in flake.lock, which suits a prototype about
        # reproducing memory-ordering behaviour better than a moving PPA.
        gcc16 = pkgs.mkShell {
          name = "hazard-pointer-prototype-gcc16";
          packages = with pkgs; [ gcc16 cmake ninja git ];
          shellHook = ''
            export CXX=g++
            export CC=gcc
          '';
        };

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

            # Interactive only -- `nix develop --command ...` is used in CI and
            # scripts, where a banner on stdout is noise that breaks parsing.
            case $- in *i*) cat <<'EOF' ;; esac
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

CPM is vendored in cmake/; the first configure still fetches GTest, so it needs
network once (cached afterwards under .cache/CPM).
EOF
          '';
        };
      });
    };
}
