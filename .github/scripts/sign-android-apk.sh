#!/usr/bin/env bash
# Align, sign and verify the final APK before CI can publish it.
set -euo pipefail

input_apk=$1
output_apk=$2
if [[ -z ${ANYKEEP_SIGNING_KEYSTORE_BASE64:-} && ${ANYKEEP_ALLOW_DEBUG_SIGNING:-false} != true ]]; then
  echo 'Persistent signing keystore is required; debug signing is allowed only for CI validation' >&2
  exit 1
fi
sdk_root=${ANDROID_SDK_ROOT:-${ANDROID_HOME:?Android SDK path is required}}
build_tools=$(find "$sdk_root/build-tools" -mindepth 1 -maxdepth 1 -type d | sort -V | tail -1)
test -x "$build_tools/zipalign"
test -x "$build_tools/apksigner"

signing_dir=$(mktemp -d)
trap 'rm -rf "$signing_dir"' EXIT
keystore="$signing_dir/signing.keystore"

if [[ -n ${ANYKEEP_SIGNING_KEYSTORE_BASE64:-} ]]; then
  : "${ANYKEEP_SIGNING_KEY_ALIAS:?Signing key alias is required}"
  : "${ANYKEEP_SIGNING_STORE_PASSWORD:?Signing store password is required}"
  : "${ANYKEEP_SIGNING_KEY_PASSWORD:?Signing key password is required}"
  printf '%s' "$ANYKEEP_SIGNING_KEYSTORE_BASE64" | base64 --decode > "$keystore"
else
  if [[ -n ${ANYKEEP_SIGNING_KEY_ALIAS:-}${ANYKEEP_SIGNING_STORE_PASSWORD:-}${ANYKEEP_SIGNING_KEY_PASSWORD:-} ]]; then
    echo 'Signing credentials were supplied without a keystore' >&2
    exit 1
  fi
  echo '::warning::APK uses a temporary Android debug key; updates across builds require a persistent signing keystore.'
  export ANYKEEP_SIGNING_KEY_ALIAS=androiddebugkey
  export ANYKEEP_SIGNING_STORE_PASSWORD=android
  export ANYKEEP_SIGNING_KEY_PASSWORD=android
  keytool -genkeypair -noprompt -keystore "$keystore" \
    -storepass:env ANYKEEP_SIGNING_STORE_PASSWORD \
    -keypass:env ANYKEEP_SIGNING_KEY_PASSWORD \
    -alias "$ANYKEEP_SIGNING_KEY_ALIAS" -keyalg RSA -keysize 2048 \
    -validity 10000 -dname 'CN=Android Debug,O=Android,C=US'
fi

"$build_tools/zipalign" -P 16 -f 4 "$input_apk" "$signing_dir/aligned.apk"
"$build_tools/apksigner" sign --ks "$keystore" \
  --ks-key-alias "$ANYKEEP_SIGNING_KEY_ALIAS" \
  --ks-pass env:ANYKEEP_SIGNING_STORE_PASSWORD \
  --key-pass env:ANYKEEP_SIGNING_KEY_PASSWORD \
  --v4-signing-enabled false --out "$output_apk" "$signing_dir/aligned.apk"
"$build_tools/zipalign" -c -P 16 4 "$output_apk"
"$build_tools/apksigner" verify --verbose --print-certs "$output_apk"
