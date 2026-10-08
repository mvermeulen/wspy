#!/usr/bin/env bash
set -euo pipefail

# Downloads an upstream llama.cpp prebuilt release (github.com/ggml-org/llama.cpp)
# for wspy-analyze --backend llama. Upstream's x64 builds carry every CPU
# variant (GGML_CPU_ALL_VARIANTS) and pick the best one at load time, so the
# same tarball works on any x86-64 host -- no local build needed.

SCRIPT_NAME=$(basename "$0")
REPO="ggml-org/llama.cpp"
VARIANT="vulkan"
TAG=""
DEST="${XDG_DATA_HOME:-$HOME/.local/share}/wspy/llama.cpp"
FORCE=0

usage(){
  cat <<EOF
Usage: $SCRIPT_NAME [--variant vulkan|rocm|cpu] [--tag bNNNNN] [--dest DIR] [--force]

Downloads a prebuilt llama.cpp release, verifies its SHA-256 against the
GitHub release metadata, and unpacks it to DEST/<tag>-<variant>, then points
DEST/current at it. wspy-analyze --backend llama finds
DEST/current/llama-server by default.

Options:
  --variant V   vulkan (default; needs only the distro Vulkan loader + Mesa),
                rocm (needs a matching ROCm userspace on the host), or
                cpu (no GPU)
  --tag T       release tag, e.g. b11514 (default: newest release that has
                the requested variant)
  --dest DIR    install root (default: $DEST)
  --force       re-download even if DEST/<tag>-<variant> already exists
  -h, --help    show this help
EOF
}

while [ $# -gt 0 ]; do
  case "$1" in
    --variant) VARIANT="$2"; shift ;;
    --tag)     TAG="$2"; shift ;;
    --dest)    DEST="$2"; shift ;;
    --force)   FORCE=1 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "$SCRIPT_NAME: unknown argument: $1" >&2; usage >&2; exit 1 ;;
  esac
  shift
done

case "$(uname -m)" in
  x86_64)  ARCH=x64 ;;
  aarch64) ARCH=arm64 ;;
  *) echo "$SCRIPT_NAME: unsupported architecture $(uname -m)" >&2; exit 1 ;;
esac

case "$VARIANT" in
  vulkan) ASSET_RE="^llama-b[0-9]+-bin-ubuntu-vulkan-${ARCH}\\.tar\\.gz\$" ;;
  rocm)   ASSET_RE="^llama-b[0-9]+-bin-ubuntu-rocm-[0-9.]+-${ARCH}\\.tar\\.gz\$" ;;
  cpu)    ASSET_RE="^llama-b[0-9]+-bin-ubuntu-${ARCH}\\.tar\\.gz\$" ;;
  *) echo "$SCRIPT_NAME: --variant must be vulkan, rocm or cpu" >&2; exit 1 ;;
esac

for tool in curl tar python3 sha256sum; do
  command -v "$tool" >/dev/null || { echo "$SCRIPT_NAME: $tool not found" >&2; exit 1; }
done

# Upstream publishes every build as a prerelease and /releases/latest points at
# an unrelated tag with no binaries, so scan the release list for the asset.
if [ -n "$TAG" ]; then
  API_URL="https://api.github.com/repos/$REPO/releases/tags/$TAG"
else
  API_URL="https://api.github.com/repos/$REPO/releases?per_page=20"
fi
read -r TAG ASSET_NAME ASSET_URL ASSET_DIGEST < <(
  curl -fsSL -H "Accept: application/vnd.github+json" "$API_URL" | python3 -c '
import json, re, sys
data = json.load(sys.stdin)
pattern = re.compile(sys.argv[1])
for rel in (data if isinstance(data, list) else [data]):
    for a in rel.get("assets", []):
        if pattern.match(a["name"]):
            print(rel["tag_name"], a["name"], a["browser_download_url"], a.get("digest") or "-")
            sys.exit(0)
sys.exit(1)
' "$ASSET_RE"
) || { echo "$SCRIPT_NAME: no $VARIANT ($ARCH) asset found in ${TAG:-recent releases}" >&2; exit 1; }

INSTALL_DIR="$DEST/$TAG-$VARIANT"
if [ -x "$INSTALL_DIR/llama-server" ] && [ "$FORCE" -eq 0 ]; then
  echo "$SCRIPT_NAME: $TAG ($VARIANT) already installed in $INSTALL_DIR"
else
  TMP_DIR=$(mktemp -d)
  trap 'rm -rf "$TMP_DIR"' EXIT
  echo "$SCRIPT_NAME: downloading $ASSET_NAME"
  curl -fL --progress-bar -o "$TMP_DIR/$ASSET_NAME" "$ASSET_URL"

  if [ "$ASSET_DIGEST" != "-" ]; then
    ACTUAL="sha256:$(sha256sum "$TMP_DIR/$ASSET_NAME" | cut -d' ' -f1)"
    if [ "$ACTUAL" != "$ASSET_DIGEST" ]; then
      echo "$SCRIPT_NAME: checksum mismatch: expected $ASSET_DIGEST, got $ACTUAL" >&2
      exit 1
    fi
    echo "$SCRIPT_NAME: checksum OK ($ASSET_DIGEST)"
  else
    echo "$SCRIPT_NAME: warning: release has no published digest, checksum not verified" >&2
  fi

  # The tarball holds one top-level llama-<tag>/ directory.
  mkdir -p "$TMP_DIR/x"
  tar -xzf "$TMP_DIR/$ASSET_NAME" -C "$TMP_DIR/x"
  SRC=$(find "$TMP_DIR/x" -mindepth 1 -maxdepth 2 -name llama-server -type f -printf '%h\n' | head -1)
  if [ -z "$SRC" ]; then
    echo "$SCRIPT_NAME: llama-server not found in $ASSET_NAME" >&2
    exit 1
  fi
  mkdir -p "$DEST"
  rm -rf "$INSTALL_DIR"
  mv "$SRC" "$INSTALL_DIR"
fi

ln -sfn "$TAG-$VARIANT" "$DEST/current"
echo "$SCRIPT_NAME: $DEST/current -> $TAG-$VARIANT"
"$DEST/current/llama-server" --version 2>&1 | grep -i '^version' || true
echo "$SCRIPT_NAME: GPUs visible to this build:"
"$DEST/current/llama-server" --list-devices 2>/dev/null | sed -n '/Available devices/,$p' | tail -n +2
