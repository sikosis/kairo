#!/bin/sh

set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 PATH_TO_KAIRO_GUI" >&2
    exit 2
fi

binary=$1
build_directory=${binary%/*}
generated_rdef="$build_directory/kairo-generated.rdef"
resource_file="$build_directory/kairo.rsrc"

cp resources/kairo.rdef "$generated_rdef"

if command -v icon2icon >/dev/null 2>&1; then
    hvif="$build_directory/kairo-icon.hvif"
    icon2icon resources/kairo-icon-master.png "$hvif" --preset icon-gradient
    {
        echo
        echo "resource vector_icon {"
        od -An -v -tx1 "$hvif" | tr -d ' \n' | tr '[:lower:]' '[:upper:]' \
            | fold -w 128 | sed 's/^/    $"/; s/$/"/'
        echo "};"
    } >> "$generated_rdef"
else
    echo "warning: hvif_tools is not installed; app metadata was embedded without the Kairo icon" >&2
    echo "warning: run 'pkgman install hvif_tools' and rebuild to embed resources/kairo-icon-master.png" >&2
fi

rc -o "$resource_file" "$generated_rdef"
xres -o "$binary" "$resource_file"
if command -v mimeset >/dev/null 2>&1; then
    mimeset -f "$binary"
fi
