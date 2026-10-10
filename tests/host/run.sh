#!/bin/sh
# Host-side tests for the eDP probe, VBT patcher and the Resizable BAR planner (needs gcc + python3, no gnu-efi).
set -e
cd "$(dirname "$0")/../.."
T=$(mktemp -d)
python3 - "$T" <<'PY'
import sys, subprocess, os
sys.path.insert(0, 'tools')
import gen_vbt_base as g
d = sys.argv[1]
open(d + '/edid.bin', 'wb').write(g.placeholder_edid())
run = lambda *a: subprocess.check_call([sys.executable, 'tools/make_vbt.py', '--edid', d + '/edid.bin', *a], stdout=subprocess.DEVNULL)
run('--lanes', '4', '--rate', 'hbr2', '--psr', 'off', '-o', d + '/base.vbt')
run('--lanes', '2', '--rate', 'hbr', '--psr', 'off', '-o', d + '/ref_a.vbt')
run('--lanes', '1', '--rate', 'rbr', '--psr', 'off', '-o', d + '/ref_b.vbt')
PY
# the C array in lib/vbt_base.c must equal a fresh generation
python3 - "$T" <<'PY'
import re, sys
c = open('lib/vbt_base.c').read()
arr = bytes(int(x, 16) for x in re.findall(r'0x([0-9A-Fa-f]{2})', c))
assert arr == open(sys.argv[1] + '/base.vbt', 'rb').read(), 'lib/vbt_base.c is stale: run tools/gen_vbt_base.py'
print('vbt_base.c up to date')
PY
gcc -Wall -Wextra -Itests/host/shim -o $T/tvp tests/host/test_vbtpatch.c lib/int_vbtpatch.c
$T/tvp $T/base.vbt $T/a.vbt 1 1 0 >/dev/null && cmp $T/a.vbt $T/ref_a.vbt && echo "patcher == make_vbt (2 lanes, HBR, PSR off)"
$T/tvp $T/base.vbt $T/b.vbt 0 0 1 >/dev/null && cmp $T/b.vbt $T/ref_b.vbt && echo "patcher == make_vbt (1 lane, RBR, PSR stays off)"
gcc -Wall -Wextra -Itests/host/shim -o $T/tedp tests/host/test_edp.c lib/int_edp.c lib/int_vbtpatch.c
$T/tedp | grep -E "^case|ALL OK"
gcc -Wall -Wextra -Itests/host/shim -o $T/trb tests/host/test_rebar.c lib/int_rebar_plan.c
$T/trb | grep -E "^case|FAIL|ALL OK"
rm -rf "$T"