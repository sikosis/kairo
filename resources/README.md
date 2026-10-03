# Kairo icon resources

`kairo-icon-master.png` is the original high-resolution concept art for Kairo's
application icon. `kairo-icon.svg` is the simplified production vector: a
cobalt-blue retro CRT monitor with a teal screen and gold compass spark, designed
to remain clear at Haiku's 16 px and 32 px icon sizes. Both contain no third-party
logo or text and are distributed under the repository's MIT license.

Haiku applications normally embed an HVIF icon. The converter is a required GUI
build dependency so a successful build always contains the icon. On Haiku,
install it and rebuild:

```sh
pkgman install hvif_tools
make clean
make gui
```

The build converts the production SVG directly to HVIF, validates the HVIF
header, adds it to a generated RDef together with `kairo.rdef`, compiles the
resources with `rc`, and attaches them to `build/kairo-gui` with `xres`. It also
writes the validated icon to the executable's `BEOS:ICON` attribute and runs
`mimeset -f` so Tracker and Deskbar refresh their metadata. The build verifies
that `BEOS:ICON` and `BEOS:APP_VERSION` exist in the finished executable and
fails instead of silently producing an iconless application. It also treats any
native `FlatIconImporter` parsing diagnostic from `mimeset` as a build failure;
an HVIF header by itself does not prove that Haiku can render the shape data.

`kairo.rdef` is a source template. The resource script reads the authoritative
two-part release number from the repository's `VERSION` file and writes matching
Haiku application metadata into the generated RDef.

For a hand-tuned icon, open the master in Icon-O-Matic and export **HVIF RDef**.
Its `resource vector_icon` block can replace the generated block.
