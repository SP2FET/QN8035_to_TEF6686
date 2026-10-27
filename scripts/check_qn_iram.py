"""Check the linked ESP32-S2 QN receive ISR's direct calls and literal references.

This checks service-path placement, not execution time. SDK fatal assertion formatting is
excluded: IDF's IRAM functions retain __func__ strings in flash for invalid-state assertions.
Indirect driver callbacks are covered by checking
both the SDK ISR and the registered on_receive entry; callback registration also checks IRAM.
Run on the car ELF, e.g. --objdump <toolchain>/xtensa-esp32s2-elf-objdump firmware.elf.
"""
import argparse
import re
import subprocess


def audit(objdump, elf):
    symbols = subprocess.check_output([objdump, "-t", str(elf)], text=True)
    headers = subprocess.check_output([objdump, "-h", str(elf)], text=True)
    sections = []
    for section, size, address in re.findall(
            r"^\s*\d+\s+(\S+)\s+([0-9a-f]+)\s+([0-9a-f]+)", headers, re.M):
        sections.append((int(address, 16), int(address, 16) + int(size, 16), section))

    def section_of(address):
        return next((name for lo, hi, name in sections if lo <= address < hi), "")
    by_name = {}
    for line in symbols.splitlines():
        parts = line.split()
        if len(parts) >= 5 and re.fullmatch(r"[0-9a-fA-F]+", parts[0]):
            section = next((p for p in parts if p.startswith(".")), "")
            by_name[parts[-1]] = (int(parts[0], 16), section)
    for name in ("on_receive", "take_reads_locked", "fill_tx_locked", "qn8035_emu_write",
                 "qn8035_emu_peek_block", "qn8035_emu_skip", "i2c_slave_isr_handler"):
        if ".iram" not in by_name.get(name, (0, ""))[1]:
            raise ValueError(f"{name} is missing or outside IRAM")
    for prefix in ("k_cal_step_khz", "k_undocumented"):
        matches = [(n, sec) for n, (_, sec) in by_name.items() if n.startswith(prefix)]
        if not matches or any(".dram" not in sec for _, sec in matches):
            raise ValueError(f"{prefix} is missing or outside DRAM")

    dis = subprocess.check_output([objdump, "-d", str(elf)], text=True)
    functions, current = {}, None
    for line in dis.splitlines():
        match = re.match(r"^([0-9a-f]+) <([^>]+)>:", line)
        if match:
            current = match[2]
            functions[current] = []
        elif current:
            functions[current].append(line)
    pending = ["on_receive", "i2c_slave_isr_handler"]
    seen, failures, assertion_names = set(), [], set()
    fatal = {"__assert_func", "__stack_chk_fail", "esp_system_abort", "abort"}
    while pending:
        name = pending.pop()
        if name in seen:
            continue
        seen.add(name)
        lines = functions.get(name, [])
        for i, line in enumerate(lines):
            for address, target in re.findall(r"\b(?:call\d*|j)\s+([0-9a-f]+) <([^>]+)>", line):
                target = target.split("+0x")[0]
                if target in fatal:
                    continue
                section = section_of(int(address, 16))
                if section.startswith(".flash"):
                    failures.append(f"{name} calls flash function {target}")
                elif target in functions:
                    pending.append(target)
            if "l32r" in line:
                # objdump annotates relocated function/data pointers held in the literal pool.
                for address, target in re.findall(r"\(([0-9a-f]+) <([^>]+)>\)", line):
                    target = target.split("+0x")[0]
                    # Xtensa windowed ABI: a10/a12/a13 are file/function/condition.
                    # Exclude only arguments in a straight-line fatal assertion block.
                    assertion_arg = False
                    if re.search(r"l32r\s+a(?:10|12|13),", line):
                        for following in lines[i + 1:i + 6]:
                            if re.search(r"call\d*\s+\w+ <__assert_func>", following):
                                assertion_arg = True
                                break
                            if not (re.search(r"\b(?:l32r|movi(?:\.n)?)\s", following) or
                                    re.search(r"\bor\s+a1,\s*a1,\s*a1\b", following)):
                                break
                    if assertion_arg:
                        assertion_names.add(name)
                        continue
                    if section_of(int(address, 16)).startswith(".flash"):
                        failures.append(f"{name} holds a flash reference to {target}")
    if failures:
        raise ValueError("\n".join(sorted(set(failures))))
    return len(seen), len(assertion_names)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf")
    parser.add_argument("--objdump", default="xtensa-esp32s2-elf-objdump")
    args = parser.parse_args()
    count, assertions = audit(args.objdump, args.elf)
    print(f"QN receive service path: {count} direct-call functions checked; code/data in RAM or ROM")
    print(f"SDK fatal assertion-name references excluded: {assertions}; no timing/bench proof")


if __name__ == "__main__":
    main()
