#!/bin/sh
# Makes the release key once and stores it as the repository's secrets, so every APK the apk workflow builds is signed with the same key and installs over the last
# one (an update). Run it once, from a terminal that has keytool and an authenticated gh:   sh kiwi/scripts/make-signing-key.sh [owner/repo]
# The key stays in ~/.kiwi-signing (keep a copy: a lost key means every player has to uninstall once); it never goes into the repository.
set -e
REPO=${1:-$(gh repo view --json nameWithOwner -q .nameWithOwner)}
D=$HOME/.kiwi-signing
[ -f "$D/kiwi-native-release.keystore" ] && { echo "$D/kiwi-native-release.keystore exists: not making another key"; exit 1; }
mkdir -p "$D"
PASS=$(head -c 24 /dev/urandom | base64 | tr -d '=+/\n' | head -c 24)
keytool -genkeypair -keystore "$D/kiwi-native-release.keystore" -alias kiwinative -keyalg RSA -keysize 4096 -validity 36500 \
  -storepass "$PASS" -keypass "$PASS" -dname "CN=Kiwi Native, O=hernandez.kiwinative"
printf 'alias=kiwinative\npassword=%s\n' "$PASS" > "$D/credentials.txt"
chmod 600 "$D/credentials.txt" "$D/kiwi-native-release.keystore"
base64 -w0 "$D/kiwi-native-release.keystore" 2>/dev/null | gh secret set KIWI_SIGN_STORE_B64 -R "$REPO" || base64 "$D/kiwi-native-release.keystore" | tr -d '\n' | gh secret set KIWI_SIGN_STORE_B64 -R "$REPO"
printf kiwinative | gh secret set KIWI_SIGN_ALIAS -R "$REPO"
printf '%s' "$PASS" | gh secret set KIWI_SIGN_PASS -R "$REPO"
echo "done: the key is in $D (back it up); secrets set on $REPO"
