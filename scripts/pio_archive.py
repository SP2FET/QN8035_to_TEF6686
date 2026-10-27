# PlatformIO post-script: archives firmware.elf after a successful USB upload, so a core dump
# of that build can be decoded later (see scripts/elf_archive.py; `--find <id>` resolves one).
# The ota target archives in scripts/ota_upload.py instead, once the device runs the build.
import os
import sys

Import("env")  # noqa: F821 (SCons global)

sys.path.insert(0, os.path.join(env.subst("$PROJECT_DIR"), "scripts"))  # noqa: F821


def archive_elf(source, target, env):  # noqa: ARG001
    try:
        import elf_archive
        elf = env.subst("$BUILD_DIR/${PROGNAME}.elf")
        print(f"ELF archived as build {elf_archive.archive(elf, elf_archive.DEFAULT_DEST, 'usb')}")
    except Exception as e:  # never fail a successful flash
        print(f"WARNING: ELF not archived: {e}")


env.AddPostAction("upload", archive_elf)  # noqa: F821
