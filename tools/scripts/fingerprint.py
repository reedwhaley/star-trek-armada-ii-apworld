"""Reproducible read-only PE fingerprint helper for the stock installation."""
import argparse, hashlib, json
import pefile

ap = argparse.ArgumentParser()
ap.add_argument("executable")
args = ap.parse_args()
data = open(args.executable, "rb").read()
pe = pefile.PE(data=data)
o = pe.OPTIONAL_HEADER
print(json.dumps({
    "size": len(data), "sha256": hashlib.sha256(data).hexdigest(),
    "md5": hashlib.md5(data).hexdigest(),
    "machine": hex(pe.FILE_HEADER.Machine), "timestamp": pe.FILE_HEADER.TimeDateStamp,
    "image_base": hex(o.ImageBase), "entry_rva": hex(o.AddressOfEntryPoint),
    "subsystem": o.Subsystem, "dll_characteristics": hex(o.DllCharacteristics),
    "sections": [{"name": s.Name.rstrip(b"\\0").decode(errors="replace"),
                  "rva": hex(s.VirtualAddress), "raw_size": s.SizeOfRawData,
                  "characteristics": hex(s.Characteristics)} for s in pe.sections]
}, indent=2))

