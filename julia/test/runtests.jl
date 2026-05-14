# Mirror of python/tests/test_smoke.py with the same fixtures and
# constants. Each Python class maps to a top-level @testset.
#
# Fixtures: small.txt  = "hello cozip\n" * 8        = 96 bytes
#           medium.bin = bytes(range(256)) * 160    = 40960 bytes

using Test
using DataFrames
using DuckDB
using Cozip


const SMALL_CONTENT  = Vector{UInt8}(repeat("hello cozip\n", 8))
const MEDIUM_CONTENT = repeat(UInt8.(0:255), 160)

const INDEX_OFFSET      = 51
const HASH_WINDOW       = 32768
const ZIP_LFH_SIG       = UInt8[0x50, 0x4B, 0x03, 0x04]
const INDEX_NAME        = Vector{UInt8}("__cozip__")
const HASH_BLOCK_HEADER = UInt8[0x0c, 0xca, 0x08, 0x00]
const INDEX_MAGIC       = Vector{UInt8}("CZIP")
const INDEX_VERSION     = UInt8[0x01, 0x00]
const PROFILE_FLAT      = 1


read_u16_le(b) = UInt16(b[1]) | (UInt16(b[2]) << 8)

read_u32_le(b) = UInt32(b[1]) | (UInt32(b[2]) << 8) |
                 (UInt32(b[3]) << 16) | (UInt32(b[4]) << 24)

function read_u64_le(b)
    val::UInt64 = 0
    for i in 1:8
        val |= UInt64(b[i]) << (8 * (i - 1))
    end
    return val
end


# UInt64 multiplication wraps mod 2^64 in Julia, so no (hi, lo) split.
function fnv1a_64(data)::UInt64
    h::UInt64 = 0xCBF29CE484222325
    prime::UInt64 = 0x100000001B3
    for b in data
        h ⊻= b
        h *= prime
    end
    return h
end


function hash_input(archive, index_size)
    archive_size = length(archive)
    suffix_start = archive_size - HASH_WINDOW
    index_end    = INDEX_OFFSET + index_size
    if index_end <= suffix_start
        return vcat(archive[INDEX_OFFSET+1:index_end],
                    archive[suffix_start+1:archive_size])
    else
        return archive[INDEX_OFFSET+1:archive_size]
    end
end

index_size_from_lfh(archive) = Int(read_u32_le(archive[19:22]))

function parse_index(payload)
    @assert payload[1:4] == INDEX_MAGIC
    n   = Int(read_u32_le(payload[8:11]))
    cur = 11

    name_lens = Int[]
    for _ in 1:n
        push!(name_lens, Int(read_u16_le(payload[cur+1:cur+2])))
        cur += 2
    end

    nms = String[]
    for nl in name_lens
        push!(nms, String(payload[cur+1:cur+nl]))
        cur += nl
    end

    offsets = UInt64[]
    for _ in 1:n
        push!(offsets, read_u64_le(payload[cur+1:cur+8]))
        cur += 8
    end

    sizes = UInt64[]
    for _ in 1:n
        push!(sizes, read_u64_le(payload[cur+1:cur+8]))
        cur += 8
    end

    return Dict(nms[i] => (offset = offsets[i], size = sizes[i]) for i in 1:n)
end

function extract_metadata_payload(archive)
    idx_size = index_size_from_lfh(archive)
    payload  = archive[INDEX_OFFSET+1:INDEX_OFFSET+idx_size]
    meta     = parse_index(payload)["__metadata__"]
    off, sz  = Int(meta.offset), Int(meta.size)
    return archive[off+1:off+sz]
end

function assert_valid_cozip(data)
    @test length(data) >= HASH_WINDOW + INDEX_OFFSET
    @test data[1:4]   == ZIP_LFH_SIG
    @test data[31:39] == INDEX_NAME
    @test data[52:55] == INDEX_MAGIC
    expected = fnv1a_64(hash_input(data, index_size_from_lfh(data)))
    stored   = read_u64_le(data[44:51])
    @test stored == expected
end


# Walks CD entries and exposes flag_bits, compress_method, lfh_offset
# and compressed_size for ZIP-compat assertions. No ZipFile.jl dep.
function parse_cd(bytes)
    n = length(bytes)
    eocd      = n - 21
    cd_size   = Int(read_u32_le(bytes[eocd+12:eocd+15]))
    cd_offset = Int(read_u32_le(bytes[eocd+16:eocd+19]))

    cur     = cd_offset + 1
    end_pos = cd_offset + cd_size
    entries = NamedTuple[]
    CD_SIG  = UInt8[0x50, 0x4B, 0x01, 0x02]

    while cur <= end_pos
        @assert bytes[cur:cur+3] == CD_SIG
        flag       = Int(read_u16_le(bytes[cur+8:cur+9]))
        method     = Int(read_u16_le(bytes[cur+10:cur+11]))
        comp_size  = Int(read_u32_le(bytes[cur+20:cur+23]))
        fn_len     = Int(read_u16_le(bytes[cur+28:cur+29]))
        ex_len     = Int(read_u16_le(bytes[cur+30:cur+31]))
        cm_len     = Int(read_u16_le(bytes[cur+32:cur+33]))
        lfh_offset = Int(read_u32_le(bytes[cur+42:cur+45]))
        fn         = String(bytes[cur+46:cur+46+fn_len-1])
        push!(entries, (
            filename        = fn,
            flag_bits       = flag,
            compress_method = method,
            compressed_size = comp_size,
            lfh_offset      = lfh_offset,
        ))
        cur += 46 + fn_len + ex_len + cm_len
    end

    return entries
end

function extract_zip_payload(bytes, filename)
    for e in parse_cd(bytes)
        if e.filename == filename
            lfh = e.lfh_offset + 1
            @assert bytes[lfh:lfh+3] == UInt8[0x50, 0x4B, 0x03, 0x04]
            fn_len        = Int(read_u16_le(bytes[lfh+26:lfh+27]))
            ex_len        = Int(read_u16_le(bytes[lfh+28:lfh+29]))
            payload_start = lfh + 30 + fn_len + ex_len
            return bytes[payload_start:payload_start + e.compressed_size - 1]
        end
    end
    error("not found in ZIP: $filename")
end


function make_fixtures()
    tmp    = mktempdir()
    small  = joinpath(tmp, "small.txt")
    medium = joinpath(tmp, "medium.bin")
    write(small,  SMALL_CONTENT)
    write(medium, MEDIUM_CONTENT)
    return (; tmp, small, medium)
end

make_input_table(fix) = DataFrame(
    name     = ["a.txt", "b.bin"],
    path     = [fix.small, fix.medium],
    category = ["text", "binary"],
)

make_paths_arg(tbl) = collect(zip(String.(tbl.name), String.(tbl.path)))

function make_archive(tmp, tbl)
    out = joinpath(tmp, "out.zip")
    Cozip.create(out, tbl)
    out
end

function archive_ctx()
    fix   = make_fixtures()
    tbl   = make_input_table(fix)
    paths = make_paths_arg(tbl)
    arc   = make_archive(fix.tmp, tbl)
    return (; fix, tbl, paths, arc, arc_bytes = read(arc))
end

function write_meta(tmp, tbl)
    pq  = joinpath(tmp, "meta.parquet")
    md  = stage_metadata(tbl).metadata
    _write_parquet(md, pq)
    return pq
end

function _write_parquet(df::DataFrame, pq_path::AbstractString)
    db  = DuckDB.DB()
    con = DBInterface.connect(db)
    try
        DuckDB.register_data_frame(con, df, "tmp_df")
        DBInterface.execute(con, "COPY tmp_df TO '$pq_path' (FORMAT parquet)")
    finally
        DBInterface.close!(con)
        DBInterface.close!(db)
    end
end


@testset "cozip" begin

    @testset "Spec invariants" begin

        @testset "archive size meets minimum" begin
            ctx = archive_ctx()
            @test filesize(ctx.arc) >= HASH_WINDOW + INDEX_OFFSET
        end

        @testset "LFH signature" begin
            ctx = archive_ctx()
            @test ctx.arc_bytes[1:4] == ZIP_LFH_SIG
        end

        @testset "index entry filename" begin
            ctx = archive_ctx()
            @test ctx.arc_bytes[31:39] == INDEX_NAME
        end

        @testset "hash block header" begin
            ctx = archive_ctx()
            @test ctx.arc_bytes[40:43] == HASH_BLOCK_HEADER
        end

        @testset "index header (magic + version + profile)" begin
            ctx = archive_ctx()
            b = ctx.arc_bytes
            @test b[52:55] == INDEX_MAGIC
            @test b[56:57] == INDEX_VERSION
            @test b[58]    == UInt8(PROFILE_FLAT)
        end

        @testset "EOCD comment is empty" begin
            ctx = archive_ctx()
            @test ctx.arc_bytes[end-1:end] == UInt8[0x00, 0x00]
        end

        @testset "integrity hash matches FNV-1a-64" begin
            ctx = archive_ctx()
            b = ctx.arc_bytes
            expected = fnv1a_64(hash_input(b, index_size_from_lfh(b)))
            stored   = read_u64_le(b[44:51])
            @test stored == expected
        end

        @testset "index lists exactly __metadata__" begin
            ctx = archive_ctx()
            b  = ctx.arc_bytes
            sz = index_size_from_lfh(b)
            entries = parse_index(b[INDEX_OFFSET+1:INDEX_OFFSET+sz])
            @test Set(keys(entries)) == Set(["__metadata__"])
            meta = entries["__metadata__"]
            @test 0 < meta.offset < length(b)
            @test 0 < meta.size  <= length(b) - meta.offset
        end
    end


    @testset "ZIP compatibility" begin

        @testset "stdlib reader lists all entries" begin
            ctx = archive_ctx()
            cd  = parse_cd(ctx.arc_bytes)
            names_set = Set([e.filename for e in cd])
            @test issubset(Set(["__cozip__", "__metadata__", "a.txt", "b.bin"]),
                           names_set)
        end

        @testset "all entries use STORE" begin
            ctx = archive_ctx()
            for e in parse_cd(ctx.arc_bytes)
                @test e.compress_method == 0
            end
        end

        @testset "no encryption, no data descriptor" begin
            ctx = archive_ctx()
            for e in parse_cd(ctx.arc_bytes)
                @test e.flag_bits & 0x01 == 0
                @test e.flag_bits & 0x08 == 0
            end
        end

        @testset "user payloads byte-exact" begin
            ctx = archive_ctx()
            @test extract_zip_payload(ctx.arc_bytes, "a.txt") == SMALL_CONTENT
            @test extract_zip_payload(ctx.arc_bytes, "b.bin") == MEDIUM_CONTENT
        end
    end


    @testset "stage_metadata" begin

        @testset "returns NamedTuple (metadata, paths)" begin
            fix = make_fixtures()
            result = stage_metadata(make_input_table(fix))
            @test result isa NamedTuple
            @test propertynames(result) == (:metadata, :paths)
            @test result.metadata isa DataFrame
            @test result.paths isa Vector{Tuple{String,String}}
        end

        @testset "paths aligned with metadata rows" begin
            fix = make_fixtures()
            md, paths = stage_metadata(make_input_table(fix))
            @test length(paths) == nrow(md)
            @test [first(p) for p in paths] == md.name
        end

        @testset "paths drive stage_create end-to-end" begin
            fix = make_fixtures()
            md, paths = stage_metadata(make_input_table(fix))
            pq  = joinpath(fix.tmp, "meta.parquet")
            _write_parquet(md, pq)
            out = joinpath(fix.tmp, "out.zip")
            stage_create(out, paths, pq)
            assert_valid_cozip(read(out))
        end

        @testset "drops path column" begin
            fix = make_fixtures()
            out = stage_metadata(make_input_table(fix)).metadata
            @test !("path" in names(out))
        end

        @testset "adds offset and size as UInt64" begin
            fix = make_fixtures()
            out = stage_metadata(make_input_table(fix)).metadata
            @test eltype(out.offset) == UInt64
            @test eltype(out.size)   == UInt64
        end

        @testset "preserves user extras" begin
            fix = make_fixtures()
            out = stage_metadata(make_input_table(fix)).metadata
            @test out.category == ["text", "binary"]
        end

        @testset "canonical column order" begin
            fix = make_fixtures()
            out = stage_metadata(make_input_table(fix)).metadata
            @test names(out)[1:3] == ["name", "offset", "size"]
        end

        @testset "sizes match source lengths" begin
            fix = make_fixtures()
            out = stage_metadata(make_input_table(fix)).metadata
            @test out.size == UInt64[length(SMALL_CONTENT), length(MEDIUM_CONTENT)]
        end

        @testset "offsets strictly increasing" begin
            fix = make_fixtures()
            out = stage_metadata(make_input_table(fix)).metadata
            @test all(diff(out.offset) .> 0)
        end

        @testset "rejects offset in input" begin
            fix = make_fixtures()
            bad = DataFrame(name = ["a.txt"], path = [fix.small], offset = [0])
            @test_throws "reserved" stage_metadata(bad)
        end

        @testset "rejects duplicate names" begin
            fix = make_fixtures()
            bad = DataFrame(name = ["x", "x"], path = [fix.small, fix.medium])
            @test_throws "duplicate" stage_metadata(bad)
        end

        @testset "rejects reserved archive name" begin
            fix = make_fixtures()
            bad = DataFrame(name = ["__metadata__"], path = [fix.small])
            @test_throws "reserved" stage_metadata(bad)
        end
    end


    @testset "stage_create" begin

        @testset "packs a valid archive" begin
            fix = make_fixtures()
            tbl = make_input_table(fix)
            pq  = write_meta(fix.tmp, tbl)
            out = joinpath(fix.tmp, "out.zip")
            stage_create(out, make_paths_arg(tbl), pq)
            assert_valid_cozip(read(out))
        end

        @testset "metadata parquet embedded verbatim" begin
            fix = make_fixtures()
            tbl = make_input_table(fix)
            pq  = write_meta(fix.tmp, tbl)
            original = read(pq)

            out = joinpath(fix.tmp, "out.zip")
            stage_create(out, make_paths_arg(tbl), pq)

            embedded = extract_metadata_payload(read(out))
            @test embedded == original
        end

        @testset "rejects parquet with path column" begin
            fix = make_fixtures()
            tbl = make_input_table(fix)
            md  = stage_metadata(tbl).metadata
            md.path = ["/tmp/a", "/tmp/b"]
            pq = joinpath(fix.tmp, "meta.parquet")
            _write_parquet(md, pq)

            @test_throws "path" stage_create(
                joinpath(fix.tmp, "out.zip"),
                make_paths_arg(tbl),
                pq,
            )
        end

        @testset "rejects parquet missing required columns" begin
            fix = make_fixtures()
            tbl = make_input_table(fix)
            bad = DataFrame(name = ["a.txt", "b.bin"])
            pq  = joinpath(fix.tmp, "meta.parquet")
            _write_parquet(bad, pq)

            @test_throws "required column" stage_create(
                joinpath(fix.tmp, "out.zip"),
                make_paths_arg(tbl),
                pq,
            )
        end

        @testset "rejects offset mismatch" begin
            fix = make_fixtures()
            tbl = make_input_table(fix)
            md  = stage_metadata(tbl).metadata
            md.offset = UInt64[0, 0]
            pq = joinpath(fix.tmp, "meta.parquet")
            _write_parquet(md, pq)

            @test_throws "offset mismatch" stage_create(
                joinpath(fix.tmp, "out.zip"),
                make_paths_arg(tbl),
                pq,
            )
        end

        @testset "rejects row count mismatch" begin
            fix = make_fixtures()
            tbl = make_input_table(fix)
            md  = stage_metadata(tbl).metadata
            truncated = md[1:1, :]
            pq = joinpath(fix.tmp, "meta.parquet")
            _write_parquet(truncated, pq)

            @test_throws "rows" stage_create(
                joinpath(fix.tmp, "out.zip"),
                make_paths_arg(tbl),
                pq,
            )
        end
    end


    @testset "schema metadata round-trip" begin

        @testset "Parquet KV metadata is preserved end-to-end" begin
            fix = make_fixtures()
            tbl = make_input_table(fix)
            md  = stage_metadata(tbl).metadata

            # Custom KV under "asterisk:geo" rather than "geo" because
            # DuckDB's spatial extension auto-validates files with the
            # canonical "geo" key and rejects ones referencing columns
            # the file does not have. The Python test uses "geo" because
            # pyarrow has no such validation.
            geo_value = string(
                "{\"version\":\"1.0.0\",\"primary_column\":\"geometry\",",
                "\"columns\":{\"geometry\":{\"encoding\":\"WKB\",\"crs\":null}}}",
            )

            pq  = joinpath(fix.tmp, "meta.parquet")
            db  = DuckDB.DB()
            con = DBInterface.connect(db)
            try
                DuckDB.register_data_frame(con, md, "tmp_md")
                sql = string(
                    "COPY tmp_md TO '$pq' (",
                    "FORMAT PARQUET, ",
                    "KV_METADATA {",
                    "'asterisk:geo': '$geo_value', ",
                    "'asterisk:tag': 'cozip-conformance'",
                    "})",
                )
                DBInterface.execute(con, sql)
            finally
                DBInterface.close!(con)
                DBInterface.close!(db)
            end

            out = joinpath(fix.tmp, "out.zip")
            stage_create(out, make_paths_arg(tbl), pq)

            embedded = extract_metadata_payload(read(out))
            rec_pq   = joinpath(fix.tmp, "rec.parquet")
            write(rec_pq, embedded)

            db  = DuckDB.DB()
            con = DBInterface.connect(db)
            try
                result = DBInterface.execute(con, string(
                    "SELECT decode(key) AS k, decode(value) AS v ",
                    "FROM parquet_kv_metadata('$rec_pq')",
                ))
                kv = DataFrame(result)
                ks = [String(k) for k in kv.k]
                vs = [String(v) for v in kv.v]
                kv_dict = Dict(zip(ks, vs))
                @test get(kv_dict, "asterisk:geo", nothing) == geo_value
                @test get(kv_dict, "asterisk:tag", nothing) == "cozip-conformance"
            finally
                DBInterface.close!(con)
                DBInterface.close!(db)
            end
        end
    end


    @testset "GeoParquet round-trip" begin
        # Loaded inside the testset so a missing GeoParquet/GeoInterface
        # fails this group cleanly without blocking the rest.
        using GeoParquet
        import GeoInterface as GI

        @testset "GeoParquet.jl reads embedded GeoParquet with geometry intact" begin
            fix = make_fixtures()
            tbl = make_input_table(fix)

            md = stage_metadata(tbl).metadata
            md.geometry = [
                GI.Point((-77.04, -12.05)),
                GI.Point(( 2.35,  48.86)),
            ]

            pq = joinpath(fix.tmp, "meta.parquet")
            GeoParquet.write(pq, md, (:geometry,))

            out = joinpath(fix.tmp, "out.zip")
            stage_create(out, make_paths_arg(tbl), pq)

            embedded = extract_metadata_payload(read(out))
            rec_pq   = joinpath(fix.tmp, "rec.parquet")
            write(rec_pq, embedded)

            recovered = GeoParquet.read(rec_pq)

            @test recovered.name == ["a.txt", "b.bin"]
            @test GI.x(recovered.geometry[1]) == -77.04
            @test GI.y(recovered.geometry[1]) == -12.05
            @test GI.x(recovered.geometry[2]) ==   2.35
            @test GI.y(recovered.geometry[2]) ==  48.86
        end
    end
end