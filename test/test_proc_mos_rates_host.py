"""Regression checks for /proc/mos percentages after many process launches."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ProcMosRatesHost(unittest.TestCase):
    def test_large_counters_and_empty_denominator(self):
        source = (ROOT / "src/proc/impl/common/mos.c").read_text()
        start = source.index("static unsigned mos_percentage(")
        end = source.index("static void mos_print_usage", start)
        probe = "#include <assert.h>\n#include <stdint.h>\n" + source[start:end] + r'''
int main(void) {
    assert(mos_percentage(0, 0) == 0);
    assert(mos_percentage(0, 100) == 0);
    assert(mos_percentage(1, 3) == 33);
    assert(mos_percentage(20000000U, 40000000U) == 50);
    assert(mos_percentage(42949672U, 42949672U) == 100);
    assert(mos_percentage(42949673U, 42949673U) == 100);
    /* The same cache efficiency must survive repeated workload batches. */
    for (unsigned batch = 1; batch <= 40; batch++)
        assert(mos_percentage(batch * 90000000U, batch * 100000000U) == 90);
    assert(mos_percentage(UINT32_MAX, UINT32_MAX) == 100);
    assert(mos_percentage(UINT32_MAX / 2, UINT32_MAX) == 49);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="mos-proc-rates-") as tmp:
            path = Path(tmp)
            (path / "probe.c").write_text(probe)
            subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                            str(path / "probe.c"), "-o", str(path / "probe")],
                           check=True, capture_output=True, text=True)
            subprocess.run([str(path / "probe")], check=True,
                           capture_output=True, text=True)
