#!/bin/bash
#
# package.sh -- build and sign the Quest APK, on Windows (Git Bash) or Linux.
#
#   bash app/android/package.sh
#
# Uses the Android SDK's build tools (aapt2, zipalign, apksigner) and
# platform android.jar, and Java's keytool. Takes the native libraries from
# build/android/lib (app/android/build.sh) and writes
# build/android/vfx1quest.apk, signed with a local debug key.

set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/../.." && pwd)
SDK=${ANDROID_SDK:-${LOCALAPPDATA:-$HOME}/Android/Sdk}
SDK=$(printf '%s\n' "$SDK" | tr '\\' '/')
BT="$SDK/build-tools/34.0.0"
JAR="$SDK/platforms/android-34/android.jar"
OUT="$ROOT/build/android"
EXE=""
[ -f "$BT/aapt2.exe" ] && EXE=".exe"

rm -f "$OUT/base.apk" "$OUT/unsigned.apk" "$OUT/vfx1quest.apk"
"$BT/aapt2$EXE" link --manifest "$HERE/AndroidManifest.xml" -I "$JAR" -o "$OUT/base.apk"

# Add the native libraries.
PY=""
for c in python3 python; do "$c" -c "" > /dev/null 2>&1 && { PY=$c; break; }; done   # skip the Windows Store stub
"$PY" - "$OUT" <<'PYTHON'
import os, sys, zipfile
out = sys.argv[1]
with zipfile.ZipFile(os.path.join(out, "base.apk")) as src, \
     zipfile.ZipFile(os.path.join(out, "unsigned.apk"), "w", zipfile.ZIP_DEFLATED) as dst:
    for item in src.infolist():
        dst.writestr(item, src.read(item.filename))
    libdir = os.path.join(out, "lib", "arm64-v8a")
    for name in sorted(os.listdir(libdir)):
        dst.write(os.path.join(libdir, name), f"lib/arm64-v8a/{name}")
        print("added", name)
PYTHON

"$BT/zipalign$EXE" -f -p 4 "$OUT/unsigned.apk" "$OUT/aligned.apk"

KEY="$OUT/debug.keystore"
if [ ! -f "$KEY" ]; then
    keytool -genkeypair -keystore "$KEY" -storepass android -keypass android -alias vfx1 \
        -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=VFX1 Quest debug" > /dev/null
fi
java -jar "$BT/lib/apksigner.jar" sign --ks "$KEY" --ks-pass pass:android --key-pass pass:android \
    --out "$OUT/vfx1quest.apk" "$OUT/aligned.apk"
rm -f "$OUT/base.apk" "$OUT/unsigned.apk" "$OUT/aligned.apk"
ls -la "$OUT/vfx1quest.apk"
