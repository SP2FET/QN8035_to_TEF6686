"""Apply the project's bounded NACK wait to a build-local ESP-IDF 5.4.0 source copy.

The installed SDK is never changed. Exact source anchors deliberately fail on SDK drift;
review/remove this workaround when changing the pinned ESP-IDF version.
"""
from pathlib import Path
import sys


def patch(source):
    entry = """static void s_i2c_send_commands(i2c_master_bus_handle_t i2c_master, TickType_t ticks_to_wait)
{
"""
    wait = """            while (i2c_ll_is_bus_busy(hal->dev)) {
                __asm__ __volatile__("nop");
            }"""
    bounded = """            while (i2c_ll_is_bus_busy(hal->dev)) {
                if (ticks_to_wait != portMAX_DELAY &&
                    (int32_t)(xTaskGetTickCount() - command_deadline) >= 0) {
                    i2c_master->cmd_idx = 0;
                    i2c_master->trans_idx = 0;
                    atomic_store(&i2c_master->status, I2C_STATUS_TIMEOUT);
                    break;
                }
                __asm__ __volatile__("nop");
            }"""
    if source.count(entry) != 1 or source.count(wait) != 1:
        raise ValueError("ESP-IDF I2C source changed; review the bounded NACK workaround")
    return source.replace(entry, entry +
                          "    const TickType_t command_deadline = xTaskGetTickCount() + ticks_to_wait;\n").replace(wait, bounded)


def main():
    src, dst = map(Path, sys.argv[1:])
    result = patch(src.read_text(encoding="utf-8"))
    if not dst.exists() or dst.read_text(encoding="utf-8") != result:
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_text(result, encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()
