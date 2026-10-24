#!/bin/sh
# Prints an SDK path for the Makefile to use on macOS, or nothing if the default SDK works.
#
# Some Command Line Tools installs ship a default SDK that their own linker cannot link against
# (the link fails with "unsupported tapi file type"). If that happens, fall back to the newest
# installed SDK that does link.
CXX=${1:-c++}
TMP=$(mktemp -d 2>/dev/null || mktemp -d -t kestrel)
trap 'rm -rf "$TMP"' EXIT
printf 'int main() { return 0; }\n' > "$TMP/t.cpp"

"$CXX" "$TMP/t.cpp" -o "$TMP/t" >/dev/null 2>&1 && exit 0

for sdk in $(ls -d /Library/Developer/CommandLineTools/SDKs/MacOSX[0-9]*.sdk 2>/dev/null | sort -r -V); do
    if SDKROOT="$sdk" "$CXX" "$TMP/t.cpp" -o "$TMP/t" >/dev/null 2>&1; then
        echo "$sdk"
        exit 0
    fi
done
