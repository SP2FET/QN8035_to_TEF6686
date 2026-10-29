# Third-party material

The license chosen for the original project code does not grant rights to material
owned by others. This inventory records known material; it is not a legal clearance.

## TEF6686 firmware patches and lookup tables

`components/tef6686/patch/tef6686_patch_v102_p224.c` and
`components/tef6686/patch/tef6686_patch_v205_p512.c` contain firmware patch and
lookup-table bytes. Their headers identify them as NXP firmware, converted byte for
byte from [PE5PVB/TEF6686_ESP32](https://github.com/PE5PVB/TEF6686_ESP32) at
commit `3a770542a029`. The headers record source and extracted-byte hashes;
`regen_patch.py` describes the conversion.

The upstream project currently declares GPL-3.0-or-later, but that declaration
does not by itself establish NXP's rights or permission to redistribute these
firmware bytes. Their applicable terms and permission are **unverified**. Obtain
and review authoritative permission before publishing the Git history, source
archives, or compiled images containing these bytes. Removing the files from the
current tree alone would not remove them from existing commits or tags.

## Other references and captures

The project uses ESP-IDF and PlatformIO dependencies under their own licenses.
The build also creates a modified build-local copy of an ESP-IDF I2C driver; see
`cmake/i2c_deadline.cmake` and `scripts/patch_idf_i2c.py`.
The original logic-analyser captures are excluded from the current tree; their
measurements and decoded test sequences remain. The captures exist in earlier Git
history, which needs separate review before publication.
The head-unit firmware files mentioned in documentation are local research inputs,
not files tracked in the current tree. Their redistribution rights are unverified.
