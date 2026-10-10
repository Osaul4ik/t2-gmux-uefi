#!/usr/bin/env python3
"""Build SSDT_IGPU.aml (mode 4) or SSDT_IGPU_BRT.aml (mode 5) with iasl.

  python tools/make_ssdt_igpu.py -o SSDT_IGPU.aml                                 (mode 4)
  python tools/make_ssdt_igpu.py --asl tools/SSDT_IGPU_BRT.asl -o SSDT_IGPU_BRT.aml   (brightness + resume, mode 5)

The .asl is only read, never written. Needs `iasl` (apt-get install acpica-tools).
"""
import argparse, os, shutil, subprocess, sys, tempfile


def die(msg):
    print("error: " + msg, file=sys.stderr)
    sys.exit(1)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--asl", default=os.path.join(here, "SSDT_IGPU.asl"), help="source (default: tools/SSDT_IGPU.asl)")
    ap.add_argument("-o", "--output", required=True, help="output .aml (do not point it into tools/)")
    ap.add_argument("--keep-asl", action="store_true", help="also write <output>.generated.asl")
    ap.add_argument("--iasl", default="iasl", help="iasl binary")
    a = ap.parse_args()

    if not shutil.which(a.iasl):
        die("iasl not found (apt-get install -y acpica-tools)")
    out_asl = open(a.asl, encoding="utf-8").read()

    out = os.path.abspath(a.output)
    if os.path.dirname(out) == os.path.abspath(here):
        die("output must not be inside tools/")
    if a.keep_asl:
        with open(out + ".generated.asl", "w", encoding="utf-8", newline="\n") as f:
            f.write(out_asl)
    with tempfile.TemporaryDirectory() as tmp:
        tasl = os.path.join(tmp, "SSDT_IGPU.asl")
        with open(tasl, "w", encoding="utf-8", newline="\n") as f:
            f.write(out_asl)
        r = subprocess.run([a.iasl, "-p", os.path.join(tmp, "SSDT_IGPU"), tasl], capture_output=True, text=True)
        sys.stdout.write(r.stdout)
        sys.stderr.write(r.stderr)
        aml = os.path.join(tmp, "SSDT_IGPU.aml")
        if r.returncode != 0 or not os.path.exists(aml):
            die("iasl failed")
        shutil.copyfile(aml, out)
    print("wrote %s (%d bytes)" % (out, os.path.getsize(out)))


if __name__ == "__main__":
    main()