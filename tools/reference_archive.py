"""Build the reference archive the site's figures measure, and print its numbers.

A million files of 1,000 bytes, 000000.tif to 999999.tif, with six metadata
columns (split, date, cloud_cover, lon, lat, landcover), written once as a plain
stored ZIP with Python's zipfile and once as a cozip with cozip.write. It prints
the Central Directory and manifest sizes, the manifest offset, and local timings
for listing the ZIP, cozip.read and a DuckDB query, and writes results.json.

    python tools/reference_archive.py OUT_DIR

It needs numpy, pyarrow, duckdb and the cozip package, and about 2.3 GB in
OUT_DIR. Timings depend on the machine; the sizes do not.
"""
import json, os, struct, sys, time, zipfile
import numpy as np
import pyarrow as pa, pyarrow.parquet as pq
import cozip, duckdb
from cozip._parquet import manifest_options

out = sys.argv[1]
N = 1_000_000
rng = np.random.default_rng(7)
src = os.path.join(out, "tile.bin")
with open(src, "wb") as f:
    f.write(rng.bytes(1000))
names = [f"{i:06d}.tif" for i in range(N)]
split = np.array(["train", "val", "test"])[np.where(np.arange(N) % 10 < 8, 0, np.where(np.arange(N) % 10 == 8, 1, 2))]
date = pa.array((np.datetime64("2024-01-01") + (np.arange(N) % 366)).astype("datetime64[D]"))
cloud = rng.uniform(0, 100, N).astype("float32")
lon = rng.uniform(-10, 30, N).round(4)
lat = rng.uniform(35, 70, N).round(4)
landcover = np.array(["forest", "water", "urban", "crop", "bare"])[rng.integers(0, 5, N)]

def ends(path):
    size = os.path.getsize(path)
    with open(path, "rb") as f:
        f.seek(size - 22); e = f.read(22)
        sig, _, _, _, total, cd_size, cd_off, clen = struct.unpack("<IHHHHIIH", e)
        assert sig == 0x06054B50 and clen == 0
        if total == 0xFFFF or cd_size == 0xFFFFFFFF or cd_off == 0xFFFFFFFF:
            f.seek(size - 22 - 20); loc = f.read(20)
            assert struct.unpack("<I", loc[:4])[0] == 0x07064B50
            z64 = struct.unpack("<Q", loc[8:16])[0]
            f.seek(z64); rec = f.read(56)
            assert struct.unpack("<I", rec[:4])[0] == 0x06064B50
            total, cd_size, cd_off = struct.unpack("<QQQ", rec[32:56])
    return {"size": size, "entries": total, "cd_size": cd_size, "cd_offset": cd_off, "end_records": size - cd_off - cd_size}

res = {}
t = time.time()
plain = os.path.join(out, "plain.zip")
data = open(src, "rb").read()
with zipfile.ZipFile(plain, "w", zipfile.ZIP_STORED) as z:
    for n in names:
        z.writestr(zipfile.ZipInfo(n, (2024, 1, 1, 0, 0, 0)), data)
res["plain"] = ends(plain); res["plain"]["write_s"] = round(time.time() - t, 1)
print("plain", res["plain"], flush=True)

table = pa.table({"name": names, "path": [src] * N, "split": split, "date": date,
                  "cloud_cover": cloud, "lon": lon, "lat": lat, "landcover": landcover})
cz = os.path.join(out, "tiles.zip")
t = time.time()
cozip.write(cz, table, temp_dir=out)
res["cozip"] = ends(cz); res["cozip"]["write_s"] = round(time.time() - t, 1)
with open(cz, "rb") as f:
    head = f.read(92)
magic, ver, prof, n = struct.unpack("<4sHBI", head[51:62])
nl = struct.unpack("<H", head[62:64])[0]
off, sz = struct.unpack("<QQ", head[64 + nl:64 + nl + 16])
res["cozip"].update({"manifest_offset": off, "manifest_size": sz, "profile": prof})
print("cozip", res["cozip"], flush=True)

# the manifest alone, and the same rows with only the three required columns,
# written with the options cozip.write uses
man = os.path.join(out, "manifest.parquet")
with open(cz, "rb") as f:
    f.seek(off); open(man, "wb").write(f.read(sz))
meta = pq.read_metadata(man)
res["manifest"] = {"rows": meta.num_rows, "columns": meta.num_columns, "row_groups": meta.num_row_groups,
                   "codec": meta.row_group(0).column(0).compression}
bare = os.path.join(out, "bare.parquet")
three = pq.read_table(man, columns=["name", "offset", "size"])
pq.write_table(three, bare, **manifest_options(three))
res["manifest"]["three_columns_size"] = os.path.getsize(bare)
print("manifest", res["manifest"], flush=True)

def best(fn, k=3):
    ts = []
    for _ in range(k):
        t = time.perf_counter(); r = fn(); ts.append(time.perf_counter() - t)
    return round(min(ts), 3), r

res["time"] = {}
res["time"]["zipfile_list_s"], cnt = best(lambda: len(zipfile.ZipFile(plain).infolist()))
res["time"]["cozip_read_s"], cnt2 = best(lambda: len(cozip.read(cz)))
con = duckdb.connect()
q = ("SELECT name, \"offset\", size FROM read_parquet(?) "
     "WHERE split = 'train' AND cloud_cover < 10 AND date >= DATE '2024-06-01' "
     "AND lon BETWEEN 0 AND 10 AND lat BETWEEN 40 AND 50")
res["time"]["duckdb_query_s"], rows = best(lambda: con.execute(q, [man]).fetchall())
res["time"]["query_rows"] = len(rows)
res["time"]["duckdb_count_by_split_s"], groups = best(lambda: con.execute("SELECT split, count(*) FROM read_parquet(?) GROUP BY ALL ORDER BY 1", [man]).fetchall())
res["time"]["groups"] = groups
res["sample_rows"] = con.execute("SELECT * FROM read_parquet(?) WHERE name IN ('000000.tif','000001.tif','000002.tif') ORDER BY name", [man]).fetchall()
res["query_first_rows"] = rows[:3]
print(json.dumps(res, indent=1, default=str))
json.dump(res, open(os.path.join(out, "results.json"), "w"), indent=1, default=str)
