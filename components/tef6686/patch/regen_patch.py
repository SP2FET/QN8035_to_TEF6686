import sys, re, hashlib

CFG = {
    "v102_p224": ("V102", "V102 p2.24", "PatchByteValues102", "LutByteValues102"),
    "v205_p512": ("V205", "V205 p5.12", "PatchByteValues205", "LutByteValues205"),
}

def body(txt, name):
    m = re.search(re.escape(name) + r"\s*\[\s*\]\s*(?:PROGMEM\s*)?=\s*\{", txt)
    s = m.end()
    e = txt.index("};", s)
    return [int(t, 16) for t in re.findall(r"0x[0-9a-fA-F]{2}\b", txt[s:e])]

def arr(name, data):
    out = "static const uint8_t %s[] = {\n" % name
    for i in range(0, len(data), 16):
        out += "    " + ", ".join("0x%02x" % b for b in data[i:i+16]) + ",\n"
    return out + "};\n"

def main(src, dst, key):
    _, label, pn, ln = CFG[key]
    raw = open(src, "rb").read()
    txt = raw.decode("latin-1")
    p, l = body(txt, pn), body(txt, ln)
    h = lambda d: hashlib.sha256(bytes(d)).hexdigest()
    fn = src.replace("\\", "/").split("/")[-1]
    c = f"""/*
 * TEF6686 (Lithio) firmware patch + LUT, {label}.
 *
 * Machine-converted, byte for byte, from PE5PVB/TEF6686_ESP32
 *   https://github.com/PE5PVB/TEF6686_ESP32 (commit 3a770542a029)
 *   src/{fn}  sha256 {hashlib.sha256(raw).hexdigest()}
 * Patch content is NXP firmware distributed with that project. Do not edit by
 * hand; regenerate from the original file and re-check the lengths below.
 *
 * patch: {len(p)} bytes  sha256 {h(p)}
 * lut:   {len(l)} bytes  sha256 {h(l)}
 */
#include "tef6686_patch.h"

{arr("s_patch", p)}
{arr("s_lut", l)}
const tef6686_patch_t tef6686_patch_{key} = {{
    .name      = "{label}",
    .patch     = s_patch,
    .patch_len = sizeof(s_patch),
    .lut       = s_lut,
    .lut_len   = sizeof(s_lut),
}};
"""
    open(dst, "w", newline="\n").write(c)
    # verify
    g = open(dst).read()
    for nm, srcn, ref in (("s_patch", p, pn), ("s_lut", l, ln)):
        blk = g[g.index(nm + "[]"):]
        blk = blk[:blk.index("};")]
        got = [int(t, 16) for t in re.findall(r"0x[0-9a-f]{2}", blk)]
        total = len(re.findall(r"0x[0-9a-fA-F]{2}\b", txt[txt.index(ref):].split("};")[0]))
        print(key, nm, "src tokens", total, "emitted", len(got), "match", got == srcn and total == len(got), "even", len(got) % 2 == 0)

if __name__ == "__main__":
    main(*sys.argv[1:4])
