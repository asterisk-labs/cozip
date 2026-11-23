using DataFrames
using Dates
using DuckDB

using .LibCozip:
    cozip_entry_t,
    cozip_error_t,
    entry_from_path,
    plan_flat!,
    write_flat!,
    INDEX_NAME,
    PADDING_NAME,
    METADATA_NAME


# Columns owned by the writer or reader; producers must never persist them.
# They are compared without case, as DuckDB, which reads manifests, does.
const _PROTECTED_LOCATION_COLUMNS = Set(["cozip:location", "taco:location"])
const _RESERVED_INPUT_COLUMNS    = union(Set(["offset", "size"]), _PROTECTED_LOCATION_COLUMNS)
const _REQUIRED_METADATA_COLUMNS = ["name", "offset", "size"]


# Shared DuckDB connection. Lifecycle owned by Cozip.__init__.
const _DUCKDB_DB   = Ref{Union{Nothing,DuckDB.DB}}(nothing)
const _DUCKDB_CON  = Ref{Union{Nothing,DuckDB.Connection}}(nothing)
const _DUCKDB_LOCK = ReentrantLock()

# Cozip only reads and writes local Parquet files, so the connection never
# installs or loads an extension.
const _DUCKDB_CONFIG = Dict(
    "autoinstall_known_extensions" => "false",
    "autoload_known_extensions" => "false",
)


function _init_duckdb!()
    _DUCKDB_CON[] === nothing || return nothing
    _DUCKDB_DB[]  = DuckDB.DB(config=_DUCKDB_CONFIG)
    _DUCKDB_CON[] = DBInterface.connect(_DUCKDB_DB[])
    return nothing
end


function _close_duckdb!()
    if _DUCKDB_CON[] !== nothing
        try
            DBInterface.close!(_DUCKDB_CON[])
        catch
        end
        _DUCKDB_CON[] = nothing
    end
    if _DUCKDB_DB[] !== nothing
        try
            DBInterface.close!(_DUCKDB_DB[])
        catch
        end
        _DUCKDB_DB[] = nothing
    end
    return nothing
end


function _duckdb_con()
    con = _DUCKDB_CON[]
    con === nothing &&
        error("cozip: DuckDB connection not initialized; Cozip.__init__ must run first")
    return con
end


"""
    stage_metadata(table) -> (metadata, paths)

Compute offsets and sizes for a cozip archive. Returns a NamedTuple
where `metadata` is a DataFrame with `name`/`offset`/`size`/extras
and `paths` is a row-aligned `Vector{Tuple{String,String}}` ready
for `stage_create`. It checks each source and reads its file size, but not its
contents.

# Arguments
- `table`: any Tables.jl-compatible source with `name` and `path`
  columns. Extras preserved; `offset`, `size` and the location columns
  are rejected in any letter case, and so are two columns whose names
  differ only in case.
"""
function stage_metadata(table)
    plan = _plan_flat(table)
    return (metadata = plan.metadata, paths = plan.paths)
end


# Checks the input table, reads each source size once and plans the
# offsets. `create` writes with these entries, so a source that changes
# before the write fails in the C writer instead of leaving a manifest
# that points at the wrong bytes.
function _plan_flat(table)
    df = _validate_input_table(table)

    names_v = String[String(n) for n in df.name]
    paths_v = String[string(p) for p in df.path]
    n_users = length(names_v)

    entries, keepalive = _plan_entries(names_v, paths_v, _source_sizes(paths_v))

    offsets = UInt64[entries[i].payload_offset for i in 1:n_users]
    sizes   = UInt64[entries[i].payload_size   for i in 1:n_users]

    out = select(df, Not(:path))
    out.name   = names_v
    out.offset = offsets
    out.size   = sizes
    rest = setdiff(names(out), _REQUIRED_METADATA_COLUMNS)
    metadata = select(out, [_REQUIRED_METADATA_COLUMNS..., rest...])

    for k in DataFrames.metadatakeys(df)
        v, style = DataFrames.metadata(df, k, style=true)
        DataFrames.metadata!(metadata, k, v; style=style)
    end

    return (
        metadata  = metadata,
        paths     = collect(zip(names_v, paths_v)),
        entries   = entries,
        keepalive = keepalive,
    )
end


"""
    stage_create(out_path, paths, metadata_parquet; validate=true) -> String

Pack a cozip archive from source files and a user-written parquet.
The parquet is embedded verbatim as `__metadata__`; cozip never
modifies it.

# Arguments
- `out_path`: destination archive.
- `paths`: iterable of `(name, source_path)` tuples, row-aligned with
  `metadata_parquet`.
- `metadata_parquet`: must contain `name` (string), `offset` and `size`
  (UInt64); must NOT contain `path` or a location column, and no two
  columns may differ only in case.
- `validate`: re-run the plan and check parquet matches.
"""
function stage_create(
    out_path,
    paths,
    metadata_parquet;
    validate::Bool = true,
)::String
    out_path_str = _out_path_arg(out_path)
    parquet_raw = _reject_nul(string(metadata_parquet), "metadata_parquet")
    isempty(parquet_raw) &&
        throw(ArgumentError("cozip: `metadata_parquet` must be a non-empty path"))
    parquet_str = abspath(parquet_raw)

    ispath(parquet_str) ||
        throw(ArgumentError("cozip: metadata parquet not found: $parquet_str"))
    isfile(parquet_str) ||
        throw(ArgumentError("cozip: metadata parquet is not a regular file: $parquet_str"))
    filesize(parquet_str) > 0 ||
        throw(ArgumentError("cozip: metadata parquet is empty"))

    _check_parquet_schema(parquet_str)

    names_v, paths_v = _validate_paths_arg(paths)
    n_users = length(names_v)

    entries, keepalive = _plan_entries(names_v, paths_v, _source_sizes(paths_v))

    if validate
        planned_offsets = UInt64[entries[i].payload_offset for i in 1:n_users]
        planned_sizes   = UInt64[entries[i].payload_size   for i in 1:n_users]
        _validate_parquet_values(parquet_str, names_v, planned_offsets, planned_sizes)
    end

    return _write_entries(out_path_str, entries, keepalive, n_users, parquet_str)
end


"""
    create(out_path, table; temp_dir=nothing) -> String

All-in-one: plan as `stage_metadata` does, write the manifest parquet
and pack the archive with the sizes that were planned, so a source that
changes in between is an error. Binary columns, `NaN`, `Inf`, `missing`,
integer widths and table-level metadata (e.g. GeoParquet `geo`) survive
the round-trip. For full control, call `stage_metadata` and
`stage_create` directly.
"""
function create(
    out_path,
    table;
    temp_dir = nothing,
)::String
    out_path_str = _out_path_arg(out_path)
    return _create_planned(out_path_str, _plan_flat(table), temp_dir)
end


function _create_planned(out_path_str::String, plan, temp_dir)::String
    tmp_dir = string(something(temp_dir, tempdir()))
    isdir(tmp_dir) || mkpath(tmp_dir)
    tmp_pq = joinpath(tmp_dir, "cozip_meta_$(getpid())_$(time_ns()).parquet")

    try
        _write_metadata_parquet(plan.metadata, tmp_pq)
        return _write_entries(out_path_str, plan.entries, plan.keepalive,
                              length(plan.paths), tmp_pq)
    finally
        rm(tmp_pq; force=true)
    end
end


function _out_path_arg(out_path)::String
    raw = _reject_nul(string(out_path), "out_path")
    isempty(raw) &&
        throw(ArgumentError("cozip: `out_path` must be a non-empty path"))
    return abspath(raw)
end


_reserved() = Set([INDEX_NAME[], PADDING_NAME[], METADATA_NAME[]])


function _reject_nul(value::String, context::String)::String
    occursin('\0', value) &&
        throw(ArgumentError("cozip: $context contains a NUL byte"))
    return value
end


function _validate_input_table(table)::DataFrame
    df = DataFrame(table; copycols=true)
    cols = names(df)
    _check_column_names(cols, "input table")

    missing_cols = sort(collect(setdiff(["name", "path"], cols)))
    isempty(missing_cols) ||
        throw(ArgumentError(
            "cozip: input table is missing required column(s): $missing_cols"
        ))

    reserved_in_input = sort([c for c in cols if lowercase(c) in _RESERVED_INPUT_COLUMNS])
    isempty(reserved_in_input) ||
        throw(ArgumentError(
            "cozip: input table must not contain reserved column(s) " *
            "$reserved_in_input; the binding computes them (names are " *
            "compared without case)"
        ))

    nrow(df) > 0 ||
        throw(ArgumentError("cozip: empty entry list"))

    reserved = _reserved()
    seen = Set{String}()
    for (i, name) in enumerate(df.name)
        ismissing(name) &&
            throw(ArgumentError("cozip: `name` column has missing at row $i"))
        name === nothing &&
            throw(ArgumentError("cozip: `name` column has nothing at row $i"))
        name isa AbstractString ||
            throw(ArgumentError(
                "cozip: row $i name must be a string, got $(typeof(name))"
            ))
        s = _reject_nul(String(name), "row $i name")
        s in reserved &&
            throw(ArgumentError("cozip: row $i uses reserved name $(repr(s))"))
        s in seen &&
            throw(ArgumentError("cozip: duplicate name $(repr(s)) at row $i"))
        push!(seen, s)
    end

    for (i, p) in enumerate(df.path)
        ismissing(p) &&
            throw(ArgumentError("cozip: `path` column has missing at row $i"))
        p === nothing &&
            throw(ArgumentError("cozip: `path` column has nothing at row $i"))
        ps = _reject_nul(string(p), "row $i path")
        _check_source(ps, "row $i ($(repr(String(df.name[i]))))")
    end

    return df
end


function _validate_paths_arg(paths)::Tuple{Vector{String},Vector{String}}
    names_v = String[]
    paths_v = String[]
    for (i, item) in enumerate(paths)
        if !(item isa Tuple) || length(item) != 2
            throw(ArgumentError(
                "cozip: paths[$i] must be a two-element (name, source_path) tuple"
            ))
        end
        name, path = item
        (ismissing(name) || name === nothing) &&
            throw(ArgumentError("cozip: paths[$i] name must not be missing"))
        (ismissing(path) || path === nothing) &&
            throw(ArgumentError("cozip: paths[$i] source must not be missing"))
        name isa AbstractString ||
            throw(ArgumentError(
                "cozip: paths[$i] name must be a string, got $(typeof(name))"
            ))
        push!(names_v, String(name))
        push!(paths_v, string(path))
    end

    isempty(names_v) &&
        throw(ArgumentError("cozip: empty paths list"))

    reserved = _reserved()
    seen = Set{String}()
    for (i, name) in enumerate(names_v)
        _reject_nul(name, "paths[$i] name")
        name in reserved &&
            throw(ArgumentError("cozip: paths[$i] uses reserved name $(repr(name))"))
        name in seen &&
            throw(ArgumentError("cozip: duplicate name $(repr(name)) at paths[$i]"))
        push!(seen, name)
    end

    for (i, p) in enumerate(paths_v)
        _reject_nul(p, "paths[$i] source")
        _check_source(p, "paths[$i] ($(repr(names_v[i])))")
    end

    return names_v, paths_v
end


# Plain messages, as in Python: a SystemError would append a stale errno.
function _check_source(path::String, context::String)
    ispath(path) ||
        throw(ArgumentError("cozip: $context: source not found: $path"))
    isfile(path) ||
        throw(ArgumentError("cozip: $context: source is not a regular file: $path"))
    return nothing
end


# Column names must stay distinct when compared without case.
function _check_column_names(cols::Vector{String}, what::String;
                             reader::String = "reads manifests")
    first_seen = Dict{String,String}()
    for c in cols
        other = get!(first_seen, lowercase(c), c)
        other == c && count(==(c), cols) > 1 &&
            throw(ArgumentError("cozip: $what has the column $(repr(c)) more than once"))
        other == c && continue
        throw(ArgumentError(
            "cozip: $what has columns that differ only in case: $(repr(other)) " *
            "and $(repr(c)); DuckDB, which $reader, cannot tell them apart"
        ))
    end
    return nothing
end


function _check_parquet_schema(parquet_path::String)
    schema = _parquet_schema(parquet_path)
    top = _top_level_rows(schema)
    cols = String[schema.name[i] for i in top]
    _check_column_names(cols, "metadata parquet")

    "path" in cols &&
        throw(ArgumentError(
            "cozip: metadata parquet must NOT contain a 'path' column. " *
            "`path` is filesystem-local and does not belong inside the " *
            "archive. Drop it before writing the parquet."
        ))

    protected = sort([c for c in cols if lowercase(c) in _PROTECTED_LOCATION_COLUMNS])
    isempty(protected) ||
        throw(ArgumentError(
            "cozip: metadata parquet must not contain reader-owned " *
            "column(s): $protected"
        ))

    missing_cols = [c for c in _REQUIRED_METADATA_COLUMNS if !(c in cols)]
    isempty(missing_cols) ||
        throw(ArgumentError(
            "cozip: metadata parquet is missing required column(s): $missing_cols"
        ))

    # Spec 13.3: name is a string, offset and size are uint64. The Parquet
    # annotations decide, as in pyarrow: DuckDB reads an unsigned INT64
    # marked only by its logical type (Parquet2.jl writes those) as BIGINT.
    row(c) = schema[top[findfirst(==(c), cols)], :]
    _is_parquet_string(row("name")) ||
        throw(ArgumentError(
            "cozip: metadata parquet column 'name' must be a string, " *
            "got $(_parquet_type_label(row("name")))"
        ))
    for c in ("offset", "size")
        _is_parquet_uint64(row(c)) ||
            throw(ArgumentError(
                "cozip: metadata parquet column '$c' must be UInt64 " *
                "(cozip spec 13.3), got $(_parquet_type_label(row(c)))"
            ))
    end
    return nothing
end


_annotation(value) = coalesce(value, "")

_is_parquet_string(row) =
    isequal(row.type, "BYTE_ARRAY") &&
    (_annotation(row.converted_type) == "UTF8" ||
     startswith(_annotation(row.logical_type), "StringType"))

function _is_parquet_uint64(row)
    isequal(row.type, "INT64") || return false
    _annotation(row.converted_type) == "UINT_64" && return true
    logical = _annotation(row.logical_type)
    return startswith(logical, "IntType(") &&
        (occursin("isSigned=0", logical) || occursin("isSigned=false", logical))
end

function _parquet_type_label(row)
    physical = _annotation(row.type)
    isempty(physical) && return "a nested column"
    annotation = _annotation(row.converted_type)
    return isempty(annotation) ? physical : "$physical ($annotation)"
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

    for field in (:name, :offset, :size)
        column = df[!, field]
        index = findfirst(ismissing, column)
        index === nothing ||
            throw(ArgumentError("cozip: metadata parquet has missing $field at row $index"))
    end

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


function _source_sizes(paths_v::Vector{String})::Vector{UInt64}
    return UInt64[filesize(p) for p in paths_v]
end


# Entries planned from sizes already read; this does not stat the sources.
# Capacity n_users + 2 leaves room for __metadata__ and the optional
# padding entry that cozip_write_flat may append.
function _plan_entries(
    names_v::Vector{String},
    paths_v::Vector{String},
    sizes::Vector{UInt64},
)
    n_users = length(names_v)
    entries, keepalive = _alloc_user_entries(names_v, paths_v, sizes, n_users + 2)
    GC.@preserve keepalive plan_flat!(entries, n_users, cozip_error_t())
    return entries, keepalive
end


# The C writer stats every source and refuses one whose size is no
# longer the planned payload size.
function _write_entries(
    out_path::String,
    entries::Vector{cozip_entry_t},
    keepalive::Vector{Vector{UInt8}},
    n_users::Integer,
    parquet::String,
)::String
    GC.@preserve keepalive write_flat!(out_path, entries, n_users, parquet, cozip_error_t())
    return out_path
end


function _alloc_user_entries(
    names_v::Vector{String},
    paths_v::Vector{String},
    sizes::Vector{UInt64},
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
            sizes[i],
            false,
            Ptr{Cvoid}(pointer(pb)),
        )
    end
    return entries, keepalive
end


# Rows of the top-level columns in DuckDB parquet_schema(). The first row
# is the root; each column is followed by its nested children, which are
# skipped by counting num_children.
function _top_level_rows(schema::DataFrame)::Vector{Int}
    nrow(schema) == 0 && return Int[]
    children = Int[coalesce(c, 0) for c in schema.num_children]
    rows = Int[]
    pos = 2
    for _ in 1:children[1]
        push!(rows, pos)
        pending = 1
        while pending > 0
            pos <= length(children) ||
                throw(ArgumentError("cozip: cannot read the manifest schema"))
            pending += children[pos] - 1
            pos += 1
        end
    end
    return rows
end


const _SCHEMA_SQL = string(
    "SELECT name, num_children, type, converted_type, logical_type ",
    "FROM parquet_schema(?)",
)


function _parquet_schema(path::String)::DataFrame
    con = _duckdb_con()
    return lock(_DUCKDB_LOCK) do
        DataFrame(DBInterface.execute(con, _SCHEMA_SQL, [path]))
    end
end


# Hive partitioning stays off, so a directory such as size=7/ never
# replaces a stored column.
function _read_parquet_subset(path::String, columns::Vector{String})::DataFrame
    con = _duckdb_con()
    return lock(_DUCKDB_LOCK) do
        # `offset` and `size` are reserved in DuckDB SQL.
        cols_sql = join((_sql_identifier(c) for c in columns), ", ")
        result = DBInterface.execute(con,
            "SELECT $cols_sql FROM read_parquet(?, hive_partitioning = false)", [path])
        DataFrame(result)
    end
end


function _table_metadata(df::DataFrame)::Dict{String,String}
    out = Dict{String,String}()
    for k in DataFrames.metadatakeys(df)
        out[string(k)] = string(DataFrames.metadata(df, k))
    end
    return out
end


function _kv_metadata_sql(md::Dict{String,String})::String
    isempty(md) && return ""
    parts = ["'$(_sql_escape(k))': '$(_sql_escape(v))'" for (k, v) in md]
    return ", KV_METADATA { " * join(parts, ", ") * " }"
end


# Element types DuckDB.register_data_frame stores, apart from missing.
const _DUCKDB_SCALAR_TYPES = (
    Bool, Int8, Int16, Int32, Int64, Int128, UInt8, UInt16, UInt32, UInt64,
    UInt128, Float32, Float64, Date, Time, DateTime,
)

function _duckdb_storable(T::Type)::Bool
    T === Union{} && return true   # a column of missing values only
    T <: AbstractString && return true
    T === Vector{UInt8} && return true
    return any(S -> T === S, _DUCKDB_SCALAR_TYPES)
end


const _ZSTD_LEVEL = 9
const _TRIAL_ROWS = 10_000

# DuckDB scans the DataFrame itself, so values keep their types: NaN, Inf,
# missing and integer widths survive. Table-level metadata becomes Parquet
# key-value metadata. Binary columns are scanned as hex text and turned
# back into BLOB with unhex(), because DuckDB.jl before 1.5.5 cannot scan a
# Vector{UInt8} column and compat allows those versions.
#
# The manifest is compressed with zstd. DuckDB cannot set an encoding per
# column, so the first rows are written as Parquet version 1 and as version
# 2, where DuckDB picks delta and byte stream split encodings itself, and
# the smaller one is used for the whole manifest.
function _write_metadata_parquet(df::DataFrame, out_path::AbstractString)
    blobs = String[]
    for c in names(df)
        T = Base.nonmissingtype(eltype(df[!, c]))
        _duckdb_storable(T) ||
            throw(ArgumentError(
                "cozip: column $(repr(c)) has element type $T, which DuckDB.jl " *
                "cannot store in Parquet; use strings, numbers, Bool, Date, " *
                "Time, DateTime or Vector{UInt8}"
            ))
        T === Vector{UInt8} && push!(blobs, c)
    end
    kv_sql = _kv_metadata_sql(_table_metadata(df))
    view = "cozip_metadata"

    scan = df
    projection = "*"
    if !isempty(blobs)
        scan = DataFrame(df; copycols=false)
        for c in blobs
            scan[!, c] = Union{Missing,String}[
                ismissing(v) ? missing : bytes2hex(v) for v in df[!, c]
            ]
        end
        projection = string("* REPLACE (", join((
            string("unhex(", _sql_identifier(c), ") AS ", _sql_identifier(c))
            for c in blobs
        ), ", "), ")")
    end

    con = _duckdb_con()
    lock(_DUCKDB_LOCK) do
        DuckDB.register_data_frame(con, scan, view)
        try
            select = string("SELECT ", projection, " FROM ", _sql_identifier(view))
            copy_to(query, path, version, extra="") = DBInterface.execute(con, string(
                "COPY (", query, ") TO '", _sql_escape(String(path)),
                "' (FORMAT parquet, COMPRESSION zstd, COMPRESSION_LEVEL ", _ZSTD_LEVEL,
                ", PARQUET_VERSION ", version, extra, ")",
            ))
            trial = string(out_path, ".trial")
            sizes = map(("V1", "V2")) do version
                try
                    copy_to(string(select, " LIMIT ", _TRIAL_ROWS), trial, version)
                    filesize(trial)
                finally
                    rm(trial; force=true)
                end
            end
            copy_to(select, out_path, sizes[2] < sizes[1] ? "V2" : "V1", kv_sql)
        finally
            DuckDB.unregister_data_frame(con, view)
        end
    end

    return nothing
end


_sql_escape(s::AbstractString) = replace(String(s), "'" => "''")
_sql_identifier(s::AbstractString) = "\"" * replace(String(s), "\"" => "\"\"") * "\""
