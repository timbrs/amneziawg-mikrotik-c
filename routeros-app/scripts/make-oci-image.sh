#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -lt 4 ]; then
    echo "usage: $0 ROOTFS OUT_TAR_GZ OCI_ARCH IMAGE_REF [OCI_VARIANT]" >&2
    exit 2
fi

rootfs=$1
out_tar_gz=$2
oci_arch=$3
image_ref=$4
oci_variant=${5:-}

tmp=$(mktemp -d)
cleanup() {
    rm -rf "$tmp"
}
trap cleanup EXIT

mkdir -p "$tmp/layout/blobs/sha256"
printf '{"imageLayoutVersion":"1.0.0"}\n' > "$tmp/layout/oci-layout"

tar --numeric-owner --owner=0 --group=0 -C "$rootfs" -cf "$tmp/layer.tar" .
layer_diff_id=$(sha256sum "$tmp/layer.tar" | awk '{print $1}')
gzip -n -9 "$tmp/layer.tar"
layer_size=$(wc -c < "$tmp/layer.tar.gz" | tr -d ' ')
layer_digest=$(sha256sum "$tmp/layer.tar.gz" | awk '{print $1}')
mv "$tmp/layer.tar.gz" "$tmp/layout/blobs/sha256/$layer_digest"

created=$(date -u +"%Y-%m-%dT%H:%M:%SZ")
python3 - "$tmp/config.json" "$oci_arch" "$created" "$layer_diff_id" <<'PY'
import json
import sys

path, arch, created, diff_id = sys.argv[1:5]
config = {
    "created": created,
    "architecture": arch,
    "os": "linux",
    "config": {
        "Entrypoint": ["/awg-routeros-app"],
        "Env": ["PATH=/"],
    },
    "rootfs": {
        "type": "layers",
        "diff_ids": [f"sha256:{diff_id}"],
    },
    "history": [
        {
            "created": created,
            "created_by": "awg-routeros-app direct OCI builder",
        }
    ],
}
with open(path, "w", encoding="utf-8") as f:
    json.dump(config, f, separators=(",", ":"))
    f.write("\n")
PY
config_size=$(wc -c < "$tmp/config.json" | tr -d ' ')
config_digest=$(sha256sum "$tmp/config.json" | awk '{print $1}')
mv "$tmp/config.json" "$tmp/layout/blobs/sha256/$config_digest"

python3 - "$tmp/manifest.json" "$config_digest" "$config_size" "$layer_digest" "$layer_size" <<'PY'
import json
import sys

path, config_digest, config_size, layer_digest, layer_size = sys.argv[1:6]
manifest = {
    "schemaVersion": 2,
    "mediaType": "application/vnd.oci.image.manifest.v1+json",
    "config": {
        "mediaType": "application/vnd.oci.image.config.v1+json",
        "digest": f"sha256:{config_digest}",
        "size": int(config_size),
    },
    "layers": [
        {
            "mediaType": "application/vnd.oci.image.layer.v1.tar+gzip",
            "digest": f"sha256:{layer_digest}",
            "size": int(layer_size),
        }
    ],
}
with open(path, "w", encoding="utf-8") as f:
    json.dump(manifest, f, separators=(",", ":"))
    f.write("\n")
PY
manifest_size=$(wc -c < "$tmp/manifest.json" | tr -d ' ')
manifest_digest=$(sha256sum "$tmp/manifest.json" | awk '{print $1}')
mv "$tmp/manifest.json" "$tmp/layout/blobs/sha256/$manifest_digest"

python3 - "$tmp/layout/index.json" "$manifest_digest" "$manifest_size" "$oci_arch" "$oci_variant" "$image_ref" <<'PY'
import json
import sys

path, manifest_digest, manifest_size, arch, variant, image_ref = sys.argv[1:7]
platform = {"architecture": arch, "os": "linux"}
if variant:
    platform["variant"] = variant
index = {
    "schemaVersion": 2,
    "manifests": [
        {
            "mediaType": "application/vnd.oci.image.manifest.v1+json",
            "digest": f"sha256:{manifest_digest}",
            "size": int(manifest_size),
            "platform": platform,
            "annotations": {
                "org.opencontainers.image.ref.name": image_ref,
            },
        }
    ],
}
with open(path, "w", encoding="utf-8") as f:
    json.dump(index, f, separators=(",", ":"))
    f.write("\n")
PY

mkdir -p "$(dirname "$out_tar_gz")"
tar -C "$tmp/layout" -cf "${out_tar_gz%.gz}" .
gzip -n -9 -f "${out_tar_gz%.gz}"
