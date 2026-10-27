"""Host fault injection against the real radio/bridge source, with mocked SDK services."""
from pathlib import Path
import importlib.util
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
FIXTURES = ROOT / "test/integration"


class FirmwareIntegrationTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def compile_run(self, sources):
        compiler = shutil.which("gcc")
        self.assertIsNotNone(compiler, "gcc is required, as for the PlatformIO native tests")
        exe = Path(self.tmp.name) / "test.exe"
        includes = [ROOT, Path(self.tmp.name), FIXTURES / "stubs",
                    *sorted((ROOT / "components").glob("*/include"))]
        cmd = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
               "-Wno-unused-parameter", "-Wno-unused-function", "-Wno-unused-variable",
               "-ffunction-sections", "-fdata-sections", *map(str, sources),
               *["-I" + str(p) for p in includes], "-Wl,--gc-sections", "-o", str(exe)]
        built = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
        self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
        ran = subprocess.run([str(exe)], capture_output=True, text=True, timeout=15)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
        return ran.stdout

    def test_radio_recovery_and_command_ordering(self):
        pure = ["af/af.c", "af/af_mem.c", "rds/rds_decoder.c", "tuning/tuning.c", "trace/trace.c"]
        out = self.compile_run([FIXTURES / "radio_test.c", *[ROOT / "components" / p for p in pure]])
        self.assertEqual(out.count("PASS "), 15, out)

    def test_bridge_restart_and_deadlines(self):
        out = self.compile_run([FIXTURES / "bridge_test.c"])
        self.assertEqual(out.count("PASS "), 4, out)

    def test_af_persistence_and_schema_reboot(self):
        out = self.compile_run([FIXTURES / "af_store_test.c", ROOT / "components/af/af.c",
                                ROOT / "components/af/af_mem.c"])
        self.assertEqual(out.count("PASS "), 3, out)

    def test_web_controls_and_bounds(self):
        node = shutil.which("node")
        self.assertIsNotNone(node, "Node.js is required for the embedded-page regression checks")
        ran = subprocess.run([node, str(FIXTURES / "web_test.js")], capture_output=True,
                             text=True, timeout=15)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)

    def test_snapshot_pair_consistency(self):
        source = (ROOT / "components/sysmon/sysmon.c").read_text(encoding="utf-8")
        start = source.index("static void snapshot(void)")
        end = source.index("static void sysmon_task", start)
        (Path(self.tmp.name) / "snapshot_under_test.inc").write_text(source[start:end],
                                                                  encoding="utf-8")
        out = self.compile_run([FIXTURES / "snapshot_test.c"])
        self.assertEqual(out.count("PASS "), 2, out)

    def test_idf_nack_deadline_and_wrap(self):
        spec = importlib.util.spec_from_file_location("i2c_patch", ROOT / "scripts/patch_idf_i2c.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        # This is the pinned source's relevant fragment, not a replacement deadline algorithm.
        source = '''static void s_i2c_send_commands(i2c_master_bus_handle_t i2c_master, TickType_t ticks_to_wait)
{
            while (i2c_ll_is_bus_busy(hal->dev)) {
                __asm__ __volatile__("nop");
            }
}
'''
        patched = module.patch(source)
        with self.assertRaises(ValueError):
            module.patch(source.replace("s_i2c_send_commands", "changed"))
        start = patched.index("            while")
        loop = patched[start:patched.rfind("}")]
        fixture = Path(self.tmp.name) / "deadline.c"
        fixture.write_text('''#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
typedef uint32_t TickType_t;
#define portMAX_DELAY UINT32_MAX
#define I2C_STATUS_TIMEOUT 9
#define atomic_store(p,v) (*(p)=(v))
static TickType_t tick;
static bool busy;
static TickType_t xTaskGetTickCount(void) { return tick++; }
static bool i2c_ll_is_bus_busy(int dev) { return busy; }
typedef struct { int cmd_idx,trans_idx,status; } master_t;
static void wait_idle(master_t *i2c_master,TickType_t ticks_to_wait) {
    struct { int dev; } value={0},*hal=&value;
    const TickType_t command_deadline=tick+ticks_to_wait;
''' + loop + '''}
int main(void) {
    master_t m={1,2,0};busy=true;tick=0;wait_idle(&m,50);
    assert(m.status==9 && !m.cmd_idx && tick<=52);
    m=(master_t){1,2,0};tick=UINT32_MAX-10;wait_idle(&m,50);
    assert(m.status==9 && tick<52);
    busy=false;m.status=0;wait_idle(&m,50);assert(m.status==0);
    return 0;
}
''', encoding="utf-8")
        self.compile_run([fixture])


if __name__ == "__main__":
    unittest.main()
