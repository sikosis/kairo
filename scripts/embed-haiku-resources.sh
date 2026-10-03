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

version=$(sed -n '1{s/[[:space:]]//g;p;}' VERSION)
case "$version" in
    ''|*[!0-9.]*|*.*.*|.*|*.)
        echo "error: VERSION must contain a two-part numeric version such as 0.45" >&2
        exit 1
        ;;
esac
version_major=${version%%.*}
version_minor=${version#*.}
sed -e "s/@KAIRO_VERSION_MAJOR@/$version_major/g" \
    -e "s/@KAIRO_VERSION_MINOR@/$version_minor/g" \
    -e "s/@KAIRO_VERSION@/$version/g" \
    resources/kairo.rdef > "$generated_rdef"

if ! command -v icon2icon >/dev/null 2>&1; then
    echo "error: hvif_tools is required to embed Kairo's application icon" >&2
    echo "error: run 'pkgman install hvif_tools', then 'make clean && make gui'" >&2
    exit 1
fi

hvif="$build_directory/kairo-icon.hvif"
icon2icon resources/kairo-icon.svg "$hvif"
hvif_header=$(od -An -N4 -tx1 "$hvif" | tr -d ' \n')
if [ "$hvif_header" != "6e636966" ]; then
    echo "error: icon2icon did not produce a valid HVIF icon" >&2
    exit 1
fi
{
    echo
    echo "resource vector_icon {"
    od -An -v -tx1 "$hvif" | tr -d ' \n' | tr '[:lower:]' '[:upper:]' \
        | fold -w 128 | sed 's/^/    $"/; s/$/"/'
    echo "};"
} >> "$generated_rdef"

rc -o "$resource_file" "$generated_rdef"
xres -o "$binary" "$resource_file"
if ! xres -l "$binary" | grep -q "BEOS:ICON"; then
    echo "error: the Kairo icon was not embedded in $binary" >&2
    exit 1
fi
if ! xres -l "$binary" | grep -q "BEOS:APP_VERSION"; then
    echo "error: the Kairo version resource was not embedded in $binary" >&2
    exit 1
fi

# Write the exact validated HVIF to the executable attribute, then refresh its
# MIME database entry. Tracker and Deskbar use these attributes/cache entries
# rather than re-reading resources on every draw.
addattr -f "$hvif" -t icon BEOS:ICON "$binary"
if ! listattr "$binary" | grep -q "BEOS:ICON"; then
    echo "error: the Kairo icon attribute was not written to $binary" >&2
    exit 1
fi
mimeset -f "$binary"
