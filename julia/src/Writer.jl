using DataFrames
using Dates
using DuckDB
using Tables

using .LibCozip:
    cozip_entry_t,
    cozip_error_t,
    entry_from_path,
    plan_flat!,
    write_flat!,
    INDEX_NAME,
    PADDING_NAME,
    METADATA_NAME


const _RESERVED_INPUT_COLUMNS    = Set(["offset", "size"])
const _REQUIRED_METADATA_COLUMNS = Set(["name", "offset", "size"])


"""
    stage_metadata(table) -> (metadata, paths)

Compute offsets and sizes for a cozip archive. Returns a NamedTuple
where `metadata` is a DataFrame with `name`/`offset`/`size`/extras
and `paths` is a row-aligned `Vector{Tuple{String,String}}` ready
for `stage_create`. Pure, no I/O.

# Arguments
- `table`: any Tables.jl-compatible source with `name` and `path`
  columns. Extras preserved; `offset` or `size` in input rejected.
"""
function stage_metadata(table)
    df = _validate_input_table(table)

    n_users = nrow(df)
    names_v = string.(df.name)
    paths_v = string.(df.path)

    entries, keepalive = _alloc_user_entries(names_v, paths_v, n_users + 1)
    err = cozip_error_t()
    GC.@preserve keepalive plan_flat!(entries, n_users, err)

    offsets = UInt64[entries[i].payload_offset for i in 1:n_users]
    sizes   = UInt64[entries[i].payload_size   for i in 1:n_users]

    out = select(df, Not(:path))
    out.offset = offsets
    out.size   = sizes
    rest = setdiff(names(out), ["name", "offset", "size"])
    metadata = select(out, ["name", "offset", "size", rest...])

    for k in DataFrames.metadatakeys(df)
        v, style = DataFrames.metadata(df, k, style=true)
        DataFrames.metadata!(metadata, k, v; style=style)
    end

    paths = collect(zip(names_v, paths_v))
    return (metadata = metadata, paths = paths)
end


"""
    stage_create(out_path, paths, metadata_parquet; validate=true) -> String

Pack a cozip archive from source files and a user-written parquet.
The parquet is embedded verbatim as `__metadata__`; cozip never reads
or modifies it.

# Arguments
- `out_path`: destination archive.
- `paths`: iterable of `(name, source_path)` tuples, row-aligned with
  `metadata_parquet`.
- `metadata_parquet`: must contain `name`, `offset`, `size`; must NOT
  contain `path`.
- `validate`: re-run the plan and check parquet matches.
"""
function stage_create(
    out_path,
    paths,
    metadata_parquet;
    validate::Bool = true,
)::String
    out_path_str = abspath(string(out_path))
    parquet_str  = abspath(string(metadata_parquet))

    isfile(parquet_str) ||
        throw(SystemError("cozip: metadata parquet not found: $parquet_str"))

    _check_parquet_schema(parquet_str)

    names_v, paths_v = _validate_paths_arg(paths)
    n_users = length(names_v)

    entries, keepalive = _alloc_user_entries(names_v, paths_v, n_users + 2)
    err = cozip_error_t()

    GC.@preserve keepalive begin
        plan_flat!(entries, n_users, err)

        if validate
            planned_offsets = UInt64[entries[i].payload_offset for i in 1:n_users]
            planned_sizes   = UInt64[entries[i].payload_size   for i in 1:n_users]
            _validate_parquet_values(parquet_str, names_v, planned_offsets, planned_sizes)
        end

        write_flat!(out_path_str, entries, n_users, parquet_str, err)
    end

    return out_path_str
end


"""
    create(out_path, table; temp_dir=nothing) -> String

All-in-one: stage_metadata + write parquet + stage_create. Binary
columns and table-level metadata (e.g. GeoParquet `geo`) survive the
round-trip. For full control, call `stage_metadata` and
`stage_create` directly.
"""
function create(
    out_path,
    table;
    temp_dir = nothing,
)::String
    metadata, paths = stage_metadata(table)

    tmp_dir = string(something(temp_dir, tempdir()))
    isdir(tmp_dir) || mkpath(tmp_dir)
    tmp_pq = joinpath(tmp_dir, "cozip_meta_$(getpid())_$(time_ns()).parquet")

    try
        _write_metadata_parquet(metadata, tmp_pq)
        return stage_create(out_path, paths, tmp_pq; validate=false)
    finally
        rm(tmp_pq; force=true)
    end
end


_reserved() = Set([INDEX_NAME[], PADDING_NAME[], METADATA_NAME[]])


function _validate_input_table(table)::DataFrame
    df = DataFrame(table; copycols=true)
    cols = names(df)

    missing_cols = sort(collect(setdiff(["name", "path"], cols)))
    isempty(missing_cols) ||
        throw(ArgumentError(
            "cozip: input table is missing required column(s): $missing_cols"
        ))

    reserved_in_input = sort(collect(intersect(_RESERVED_INPUT_COLUMNS, Set(cols))))
    isempty(reserved_in_input) ||
        throw(ArgumentError(
            "cozip: input table must not contain reserved column(s) " *
            "$reserved_in_input; the binding computes them"
        ))

    nrow(df) > 0 ||
        throw(ArgumentError("cozip: empty entry list"))

    reserved = _reserved()
    seen = Set{String}()
    for (i, name) in enumerate(df.name)
        s = string(name)
        s in reserved &&
            throw(ArgumentError("cozip: row $i uses reserved name $(repr(s))"))
        s in seen &&
            throw(ArgumentError("cozip: duplicate name $(repr(s)) at row $i"))
        push!(seen, s)
    end

    for (i, p) in enumerate(df.path)
        ps = string(p)
        isfile(ps) ||
            throw(SystemError(
                "cozip: row $i ($(repr(string(df.name[i])))): source not found: $ps"
            ))
    end

    return df
end


function _validate_paths_arg(paths)::Tuple{Vector{String},Vector{String}}
    names_v = String[]
    paths_v = String[]
    for p in paths
        push!(names_v, string(first(p)))
        push!(paths_v, string(last(p)))
    end

    isempty(names_v) &&
        throw(ArgumentError("cozip: empty paths list"))

    reserved = _reserved()
    seen = Set{String}()
    for (i, name) in enumerate(names_v)
        name in reserved &&
            throw(ArgumentError("cozip: paths[$i] uses reserved name $(repr(name))"))
        name in seen &&
            throw(ArgumentError("cozip: duplicate name $(repr(name)) at paths[$i]"))
        push!(seen, name)
    end

    for (i, p) in enumerate(paths_v)
        isfile(p) ||
            throw(SystemError(
                "cozip: paths[$i] ($(repr(names_v[i]))): source not found: $p"
            ))
    end

    return names_v, paths_v
end


function _check_parquet_schema(parquet_path::String)
    cols = _read_parquet_columns(parquet_path)

    "path" in cols &&
        throw(ArgumentError(
            "cozip: metadata parquet must NOT contain a 'path' column. " *
            "`path` is filesystem-local and does not belong inside the " *
            "archive. Drop it before writing the parquet."
        ))

    missing_cols = sort(collect(setdiff(_REQUIRED_METADATA_COLUMNS, Set(cols))))
    isempty(missing_cols) ||
        throw(ArgumentError(
            "cozip: metadata parquet is missing required column(s): $missing_cols"
        ))
end


function _validate_parquet_values(
    parquet_path::String,
    expected_names::Vector{String},
    expected_offsets::Vector{UInt64},
    expected_sizes::Vector{UInt64},
)
    df = _read_parquet_subset(parquet_path, ["name", "offset", "size"])

    nrow(df) == length(expected_names) ||
        throw(ArgumentError(
            "cozip: metadata parquet has $(nrow(df)) rows, " *
            "paths has $(length(expected_names))"
        ))

    pq_names   = String.(df.name)
    pq_offsets = UInt64.(df.offset)
    pq_sizes   = UInt64.(df.size)

    if pq_names != expected_names
        i = findfirst(pq_names .!= expected_names)
        throw(ArgumentError(
            "cozip: name mismatch at row $i: " *
            "parquet=$(repr(pq_names[i])), paths=$(repr(expected_names[i]))"
        ))
    end

    if pq_offsets != expected_offsets
        i = findfirst(pq_offsets .!= expected_offsets)
        throw(ArgumentError(
            "cozip: offset mismatch at row $i ($(repr(expected_names[i]))): " *
            "parquet=$(pq_offsets[i]), plan=$(expected_offsets[i])"
        ))
    end

    if pq_sizes != expected_sizes
        i = findfirst(pq_sizes .!= expected_sizes)
        throw(ArgumentError(
            "cozip: size mismatch at row $i ($(repr(expected_names[i]))): " *
            "parquet=$(pq_sizes[i]), plan=$(expected_sizes[i])"
        ))
    end
end


function _alloc_user_entries(
    names_v::Vector{String},
    paths_v::Vector{String},
    capacity::Integer,
)::Tuple{Vector{cozip_entry_t},Vector{Vector{UInt8}}}
    n_users = length(names_v)
    @assert capacity >= n_users
    entries = Vector{cozip_entry_t}(undef, capacity)
    keepalive = Vector{Vector{UInt8}}(undef, 2 * n_users)
    for i in 1:n_users
        nb = Vector{UInt8}(codeunits(names_v[i])); push!(nb, 0x00)
        pb = Vector{UInt8}(codeunits(paths_v[i])); push!(pb, 0x00)
        keepalive[2i - 1] = nb
        keepalive[2i]     = pb

        entries[i] = entry_from_path(
            Ptr{Cchar}(pointer(nb)),
            UInt64(filesize(paths_v[i])),
            false,
            Ptr{Cvoid}(pointer(pb)),
        )
    end
    return entries, keepalive
end


# Cached: short-lived connections trigger Windows heap corruption (duckdb#10787).
const _DUCKDB_DB  = Ref{Any}(nothing)
const _DUCKDB_CON = Ref{Any}(nothing)

function _duckdb_con()
    if _DUCKDB_CON[] === nothing
        _DUCKDB_DB[]  = DuckDB.DB()
        _DUCKDB_CON[] = DBInterface.connect(_DUCKDB_DB[])
    end
    return _DUCKDB_CON[]
end


function _read_parquet_columns(path::String)::Vector{String}
    result = DBInterface.execute(_duckdb_con(),
        "SELECT * FROM read_parquet('$(_sql_escape(path))') LIMIT 0")
    return names(DataFrame(result))
end

function _read_parquet_subset(path::String, columns::Vector{String})::DataFrame
    # `offset` and `size` are reserved in DuckDB SQL.
    cols_sql = join(["\"$c\"" for c in columns], ", ")
    result = DBInterface.execute(_duckdb_con(),
        "SELECT $cols_sql FROM read_parquet('$(_sql_escape(path))')")
    return DataFrame(result)
end


function _table_metadata(df::DataFrame)::Dict{String,String}
    out = Dict{String,String}()
    for k in DataFrames.metadatakeys(df)
        out[string(k)] = string(DataFrames.metadata(df, k))
    end
    return out
end


function _julia_to_duckdb_type(T::Type)::String
    if T isa Union && Missing <: T
        return _julia_to_duckdb_type(Base.nonmissingtype(T))
    end
    T == Bool     && return "BOOLEAN"
    T == Int8     && return "TINYINT"
    T == Int16    && return "SMALLINT"
    T == Int32    && return "INTEGER"
    T == Int64    && return "BIGINT"
    T == UInt8    && return "UTINYINT"
    T == UInt16   && return "USMALLINT"
    T == UInt32   && return "UINTEGER"
    T == UInt64   && return "UBIGINT"
    T == Float32  && return "REAL"
    T == Float64  && return "DOUBLE"
    T == String   && return "VARCHAR"
    T == Date     && return "DATE"
    T == DateTime && return "TIMESTAMP"
    T <: AbstractVector{UInt8} && return "BLOB"
    return "VARCHAR"
end


function _kv_metadata_sql(md::Dict{String,String})::String
    isempty(md) && return ""
    parts = ["'$(_sql_escape(k))': '$(_sql_escape(v))'" for (k, v) in md]
    return ", KV_METADATA { " * join(parts, ", ") * " }"
end


# unhex(): DuckDB's `'\xNN'::BLOB` only consumes 2 hex chars per escape.
function _sql_literal(v)::String
    v === missing  && return "NULL"
    v === nothing  && return "NULL"
    v isa Bool     && return v ? "TRUE" : "FALSE"
    v isa AbstractVector{UInt8} && return "unhex('" * bytes2hex(v) * "')"
    v isa AbstractString && return "'" * _sql_escape(string(v)) * "'"
    v isa Date     && return "DATE '" * string(v) * "'"
    v isa DateTime && return "TIMESTAMP '" * replace(string(v), 'T' => ' ') * "'"
    v isa Integer || v isa AbstractFloat && return string(v)
    return "'" * _sql_escape(string(v)) * "'"
end


function _write_metadata_parquet(df::DataFrame, out_path::AbstractString)
    cols      = names(df)
    col_types = [string("\"", c, "\" ", _julia_to_duckdb_type(eltype(df[!, c])))
                 for c in cols]
    tbl = "tmp_cozip_$(getpid())_$(time_ns())"
    con = _duckdb_con()

    try
        DBInterface.execute(con,
            "CREATE TABLE $tbl ($(join(col_types, ", ")))")
        for i in 1:nrow(df)
            vals = join((_sql_literal(df[i, c]) for c in cols), ", ")
            DBInterface.execute(con, "INSERT INTO $tbl VALUES ($vals)")
        end
        kv_sql = _kv_metadata_sql(_table_metadata(df))
        DBInterface.execute(con,
            "COPY $tbl TO '$(_sql_escape(String(out_path)))' (FORMAT parquet$kv_sql)")
    finally
        DBInterface.execute(con, "DROP TABLE IF EXISTS $tbl")
    end
    return nothing
end


_sql_escape(s::AbstractString) = replace(String(s), "'" => "''")