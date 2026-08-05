#!/bin/sh
set -eu

backend_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
model_dir=${ARRAKIS_FINBERT_DIR:-"$backend_root/models/finbert"}
model_url=${ARRAKIS_FINBERT_MODEL_URL:-}
vocab_url=${ARRAKIS_FINBERT_VOCAB_URL:-}
model_sha=${ARRAKIS_FINBERT_MODEL_SHA256:-4a8d58ba2f8d74c7fca30fdb49fbbe367b64760104b64e8623e896a007229a6e}
vocab_sha=${ARRAKIS_FINBERT_VOCAB_SHA256:-07eced375cec144d27c900241f3e339478dec958f92fddbc551f295c992038a3}

if [ -z "$model_url" ] || [ -z "$vocab_url" ]; then
    echo "Set ARRAKIS_FINBERT_MODEL_URL and ARRAKIS_FINBERT_VOCAB_URL to the versioned exact Arrakis export." >&2
    exit 2
fi

checksum() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    else
        shasum -a 256 "$1" | awk '{print $1}'
    fi
}

download_verified() {
    url=$1
    expected=$2
    destination=$3
    temporary="$destination.part"
    mkdir -p "$(dirname -- "$destination")"
    rm -f "$temporary"
    curl --fail --location --retry 5 --retry-delay 2 --retry-all-errors --silent --show-error "$url" --output "$temporary"
    actual=$(checksum "$temporary")
    if [ "$actual" != "$expected" ]; then
        echo "Checksum mismatch for $destination: expected $expected, got $actual" >&2
        rm -f "$temporary"
        exit 1
    fi
    mv "$temporary" "$destination"
}

download_verified "$model_url" "$model_sha" "$model_dir/model.onnx"
download_verified "$vocab_url" "$vocab_sha" "$model_dir/vocab.txt"
echo "Fetched and verified Arrakis FinBERT artifacts in $model_dir"
