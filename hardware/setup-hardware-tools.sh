#!/usr/bin/env bash
# Install the headless KiCad toolchain the hardware/ scripts use (see
# hardware/AGENTS.md, "Tools"):
#
#   - KiCad 9 from the KiCad PPA (kicad-cli in /usr/bin, and the pcbnew
#     Python module for /usr/bin/python3). Not the snap: it has no Python
#     module and is missing ICU.
#   - Freerouting 2.5.0, the autorouter: ~/opt/freerouting/freerouting.jar
#   - a portable Java 25 (Eclipse Temurin JRE) for it, which needs class
#     file version 69: ~/opt/jre25
#
#   hardware/setup-hardware-tools.sh [kicad|freerouting|java|all]
#
# Ubuntu only for the KiCad step (it adds ppa:kicad/kicad-9.0-releases and
# runs apt with sudo). Each step skips itself when its tool is already
# there; rerun after a failure. Versions are pinned below; override with the
# environment variables (FREEROUTING_VERSION, TEMURIN_VERSION, OPT_DIR).
set -euo pipefail

OPT_DIR="${OPT_DIR:-$HOME/opt}"
FREEROUTING_VERSION="${FREEROUTING_VERSION:-2.5.0}"
# The JRE miner6 runs (25.0.5+8); "latest" would drift.
TEMURIN_VERSION="${TEMURIN_VERSION:-25.0.5+8}"
KICAD_PPA="ppa:kicad/kicad-9.0-releases"

die() { printf 'setup-hardware-tools: %s\n' "$*" >&2; exit 1; }
say() { printf '== %s\n' "$*"; }

sudo_cmd() {
    if ((EUID == 0)); then "$@"; else sudo "$@"; fi
}

install_kicad() {
    if command -v kicad-cli >/dev/null 2>&1 && /usr/bin/python3 -c 'import pcbnew' >/dev/null 2>&1; then
        say "KiCad: already installed ($(kicad-cli version 2>/dev/null | head -1)), pcbnew importable"
        return
    fi
    if command -v snap >/dev/null 2>&1 && snap list kicad >/dev/null 2>&1; then
        printf 'setup-hardware-tools: note: the KiCad snap is installed; the scripts need the PPA build\n' >&2
        printf '  (/usr/bin/kicad-cli and pcbnew for /usr/bin/python3). Consider: sudo snap remove kicad\n' >&2
    fi
    [[ -r /etc/os-release ]] && . /etc/os-release
    [[ "${ID:-}" == ubuntu ]] || die "KiCad step supports Ubuntu (the KiCad PPA) only; found ${ID:-unknown}. Install KiCad 9 with its Python module by hand."
    say "KiCad: adding $KICAD_PPA and installing kicad (sudo)"
    command -v add-apt-repository >/dev/null 2>&1 || sudo_cmd apt-get install -y software-properties-common
    sudo_cmd add-apt-repository -y "$KICAD_PPA"
    sudo_cmd apt-get update
    sudo_cmd apt-get install -y kicad
    command -v kicad-cli >/dev/null 2>&1 || die "kicad-cli not on PATH after install"
    /usr/bin/python3 -c 'import pcbnew' >/dev/null 2>&1 || die "pcbnew not importable by /usr/bin/python3 after install"
    say "KiCad: $(kicad-cli version | head -1)"
}

install_freerouting() {
    local dir="$OPT_DIR/freerouting" jar="freerouting-$FREEROUTING_VERSION.jar"
    if [[ -s "$dir/$jar" ]]; then
        say "Freerouting: $dir/$jar already there"
    else
        say "Freerouting: downloading $FREEROUTING_VERSION"
        mkdir -p "$dir"
        curl -fsSL -o "$dir/$jar.part" \
            "https://github.com/freerouting/freerouting/releases/download/v$FREEROUTING_VERSION/$jar"
        mv "$dir/$jar.part" "$dir/$jar"
    fi
    ln -sfn "$jar" "$dir/freerouting.jar"
}

install_java() {
    local dir="$OPT_DIR/jre25"
    if [[ -x "$dir/bin/java" ]]; then
        say "Java: $("$dir/bin/java" -version 2>&1 | head -1) already in $dir"
        return
    fi
    local arch
    case "$(uname -m)" in
        x86_64) arch=x64 ;;
        aarch64|arm64) arch=aarch64 ;;
        *) die "no Temurin build mapped for $(uname -m)" ;;
    esac
    say "Java: downloading Temurin JRE $TEMURIN_VERSION ($arch)"
    local tmp
    tmp="$(mktemp -d)"
    trap 'rm -rf "$tmp"' RETURN
    curl -fsSL -o "$tmp/jre.tar.gz" \
        "https://api.adoptium.net/v3/binary/version/jdk-${TEMURIN_VERSION//+/%2B}/linux/$arch/jre/hotspot/normal/eclipse"
    mkdir -p "$tmp/jre"
    tar -xzf "$tmp/jre.tar.gz" -C "$tmp/jre" --strip-components=1
    "$tmp/jre/bin/java" -version >/dev/null 2>&1 || die "downloaded JRE doesn't run"
    mkdir -p "$OPT_DIR"
    mv "$tmp/jre" "$dir"
    say "Java: $("$dir/bin/java" -version 2>&1 | head -1)"
}

check_freerouting() {
    local java="$OPT_DIR/jre25/bin/java"
    [[ -x "$java" && -s "$OPT_DIR/freerouting/freerouting.jar" ]] || return 0
    say "Freerouting: checking it starts under $("$java" -version 2>&1 | head -1)"
    if timeout 120 "$java" -jar "$OPT_DIR/freerouting/freerouting.jar" --help 2>&1 | grep -qi "usage\|-de\b\|--gui"; then
        say "Freerouting: OK"
    else
        die "Freerouting didn't start (try: $java -jar $OPT_DIR/freerouting/freerouting.jar --help)"
    fi
}

case "${1:-all}" in
    kicad) install_kicad ;;
    freerouting) install_freerouting; check_freerouting ;;
    java) install_java ;;
    all)
        command -v curl >/dev/null 2>&1 || die "curl is required"
        install_kicad
        install_freerouting
        install_java
        check_freerouting
        say "Done. tools/autoroute.sh finds these by default (FREEROUTING_JAR, JAVA override)."
        ;;
    -h|--help) sed -n '2,19p' "$0" | sed 's/^# \{0,1\}//' ;;
    *) die "usage: $0 [kicad|freerouting|java|all]" ;;
esac
