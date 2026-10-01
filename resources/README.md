# Kairo icon resources

`kairo-icon-master.png` is the original high-resolution, transparent source for
Kairo's application icon. It depicts a vivid cobalt-blue retro CRT monitor with a teal
screen and compass spark, using a dimensional late-1990s BeOS-inspired style.
It contains no third-party logo or text and is distributed under the repository's
MIT license.

Haiku applications normally embed an HVIF icon. On Haiku, install the converter
and rebuild:

```sh
pkgman install hvif_tools
make clean
make gui
```

The build converts the PNG with `icon2icon --preset icon-gradient`, adds it to a
generated RDef together with `kairo.rdef`, compiles the resources with `rc`, and
attaches them to `build/kairo-gui` with `xres`. If `hvif_tools` is unavailable,
the application metadata is still embedded and the build prints a warning.

For a hand-tuned icon, open the master in Icon-O-Matic and export **HVIF RDef**.
Its `resource vector_icon` block can replace the generated block.
