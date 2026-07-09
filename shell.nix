# shell.nix — reproducible dev environment for the STICKER firmware.
#
# Usage:
#   nix-shell              # first entry runs west init/update (downloads Zephyr)
#   cd app && make         # build release firmware -> build/zephyr/zephyr.hex
#
# Set STICKER_SKIP_BOOTSTRAP=1 to skip the pip/west steps on entry.

let
  # --- Pinned nixpkgs (matches host system rev, dated 2026-07-07) ------------
  nixpkgs = builtins.fetchTarball {
    url = "https://github.com/NixOS/nixpkgs/archive/0ad6f47ea4fe188f4bc8f0380f93ae8523337c6c.tar.gz";
    sha256 = "07lxgdh4cgvv4bl964vq0y2y6dsw44nlg9hgmhxsxajbsphk44nk";
  };

  pkgs = import nixpkgs {
    config = {
      allowUnfree = true; # SEGGER J-Link is unfree
      permittedInsecurePackages = [ "segger-jlink-qt4-874" ];
      segger-jlink.acceptLicense = true;
    };
  };

  inherit (pkgs) lib;

  # GNU Arm Embedded toolchain (official ARM arm-none-eabi build, repackaged by
  # nixpkgs — already runs natively on NixOS, no autoPatchelf needed). Zephyr
  # consumes it through the `gnuarmemb` variant below.
  armToolchain = pkgs.gcc-arm-embedded;

  # Libraries needed at runtime by manylinux pip wheels (grpcio-tools,
  # clang-format, rttt) so they load under a NixOS-provided interpreter.
  wheelLibs = lib.makeLibraryPath [ pkgs.stdenv.cc.cc.lib pkgs.zlib pkgs.segger-jlink ];
in
pkgs.mkShell {
  name = "sticker-fw";

  packages = with pkgs; [
    # ARM cross toolchain (arm-none-eabi-*)
    armToolchain
    # Zephyr / CMake build system
    cmake
    ninja
    gnumake
    dtc
    gperf
    # Host Python (venv is built from this)
    python312
    # General build/fetch helpers
    git
    wget
    which
    xz
    file
    # Flashing / debug — J-Link over SWD for STM32WLE5CC
    segger-jlink
  ];

  # Use our own toolchain instead of the Zephyr SDK. gnuarmemb expects
  # $GNUARMEMB_TOOLCHAIN_PATH/bin/arm-none-eabi-gcc to exist.
  ZEPHYR_TOOLCHAIN_VARIANT = "gnuarmemb";
  GNUARMEMB_TOOLCHAIN_PATH = "${armToolchain}";

  # nix-ld support so prebuilt pip wheels find their loader/libs on NixOS.
  NIX_LD = lib.fileContents "${pkgs.stdenv.cc}/nix-support/dynamic-linker";
  NIX_LD_LIBRARY_PATH = wheelLibs;

  shellHook = ''
    export LD_LIBRARY_PATH="${wheelLibs}''${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

    # Bootstrap the west workspace + Python venv. Wrapped in a function so a
    # failure never kills the interactive shell (no `set -e` leakage).
    sticker_bootstrap() {
      local repo ws venv req stamp want
      repo="$PWD"
      ws="$(dirname "$repo")"          # workspace root = parent (west self.path=sticker)
      venv="$ws/.venv"

      # 1) Python virtual environment ---------------------------------------
      if [ ! -x "$venv/bin/python" ]; then
        echo "[sticker] creating Python venv at $venv"
        python3 -m venv "$venv" || { echo "[sticker] venv creation failed"; return 1; }
      fi
      # shellcheck disable=SC1091
      source "$venv/bin/activate"

      if [ -n "''${STICKER_SKIP_BOOTSTRAP:-}" ]; then
        echo "[sticker] STICKER_SKIP_BOOTSTRAP set — skipping pip/west bootstrap"
        return 0
      fi

      # 2) Python dependencies (reinstall only when the dev reqs change) ------
      req="$repo/scripts/west_commands/requirements-dev.txt"
      stamp="$venv/.sticker-pip-stamp"
      want="$(sha256sum "$req" | cut -d' ' -f1)"
      if [ "$(cat "$stamp" 2>/dev/null)" != "$want" ]; then
        echo "[sticker] installing Python dependencies (west, rttt, protobuf, dev reqs)..."
        pip install --quiet --upgrade pip                        || return 1
        pip install --quiet west rttt protobuf grpcio-tools      || return 1
        pip install --quiet -r "$req"                            || return 1
        echo "$want" > "$stamp"
      fi

      # 3) west workspace ----------------------------------------------------
      if [ ! -d "$ws/.west" ]; then
        echo "[sticker] initializing west workspace at $ws"
        west init -l "$repo" || return 1
      fi
      if [ ! -d "$ws/zephyr" ]; then
        echo "[sticker] west update — fetching the Zephyr tree (first time only, may take minutes)..."
        ( cd "$ws" && west update )               || return 1
        ( cd "$ws" && west zephyr-export )         || true
        ( cd "$ws" && west packages pip --install )|| true   # Zephyr's own Python deps
      fi
      return 0
    }

    if sticker_bootstrap; then
      echo "[sticker] environment ready — arm-none-eabi (gnuarmemb) + west + J-Link."
      echo "[sticker] build with:  cd app && make        (debug: make debug, flash: make flash)"
    else
      echo "[sticker] bootstrap incomplete — see errors above. Fix and re-enter the shell."
    fi
    unset -f sticker_bootstrap
  '';
}
