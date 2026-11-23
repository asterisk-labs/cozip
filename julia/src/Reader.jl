using DataFrames
using DuckDB

using .LibCozip: read_flat_metadata, location_base, verify_archive


# Forward declaration: claims `read` as a name owned by Cozip
function read end


"""
    verify(source) -> nothing

Check a cozip archive's access path, whatever its profile: the header,
index, indexed names and ranges, integrity hash and ZIP end record (spec
8.5 and 7). It does not parse the Central Directory, check payloads or
enforce profile rules.

Returns `nothing` when these checks pass. A failed check throws
`Cozip.LibCozip.CozipError`, named after the rule, such as
`HASH_MISMATCH`. Not supported on Windows yet, like `Cozip.read`.

# Arguments
- `source`: local path or http(s)/s3/gcs/azure/hf URL to the `.zip`.
"""
function verify(source)::Nothing
    _check_reader_platform("Cozip.verify")
    verify_archive(_check_source(source))
    return nothing
end


function _check_reader_platform(name::String)
    Sys.iswindows() || return nothing
    error("""
    cozip: $name is not supported on Windows yet.

    The native reader has not been verified on Windows. Writer
    operations (Cozip.create, stage_metadata, stage_create) work
    fine. For reads, use Linux, macOS, WSL, or the Python binding.
    """)
end


function _check_source(source)::String
    src = string(source)
    isempty(src) &&
        throw(ArgumentError("cozip: `source` must be a non-empty string"))
    occursin('\0', src) &&
        throw(ArgumentError("cozip: `source` contains a NUL byte"))
    isascii(src) ||
        throw(ArgumentError(
            "cozip: `source` contains non-ASCII characters; " *
            "cozip supports ASCII paths and URLs only",
        ))
    return src
end


# Stored columns the reader never returns, compared without case: the
# reader-owned location names (spec 12.3) and the former location column.
const _DROPPED_MANIFEST_COLUMNS = Set(["cozip:location", "taco:location", "cozip:gdal_vsi"])


"""
    read(source; columns=nothing, location=true) -> DataFrame

Read the manifest of a FLAT-profile cozip archive. The byte-0 index is
read natively and only the `__metadata__` Parquet manifest is fetched;
DuckDB decodes it without loading any extension. Stored `cozip:location`,
`taco:location` and `cozip:gdal_vsi` columns are dropped in any letter
case, and `cozip:location` is computed again.

Archives using any profile other than Flat (`profile = 1`), including TACO
(`profile = 2`), are rejected with a `CozipError`.

Not supported on Windows yet. The writer is.

# Arguments
- `source`: local path or http(s)/s3/gcs/azure/hf URL to the `.zip`.
- `columns`: `nothing` returns every manifest column. Otherwise extra
  column names, matched exactly; `name`, `offset` and `size` come first,
  then `cozip:location` when enabled, then the extras in the order given.
  An unknown name is an error; location names are ignored in any case.
- `location`: include the computed `cozip:location` column.
"""
function read(
    source;
    columns::Union{Nothing,AbstractVector{<:AbstractString}} = nothing,
    location::Bool = true,
)::DataFrame
    _check_reader_platform("Cozip.read")
    src = _check_source(source)

    if columns !== nothing && any(isempty, columns)
        throw(ArgumentError("cozip: `columns` entries must be non-empty"))
    end

    metadata = read_flat_metadata(src)
    base = location ? location_base(src) : ""
    label = occursin("://", src) ? "remote archive" : src

    return mktempdir(prefix="cozip-") do dir
        path = joinpath(dir, "__metadata__.parquet")
        Base.write(path, metadata)
        con = _duckdb_con()
        lock(_DUCKDB_LOCK) do
            query(sql, params) = try
                DataFrame(DBInterface.execute(con, sql, params))
            catch err
                (err isa DuckDB.QueryException && occursin(path, err.var)) || rethrow()
                throw(DuckDB.QueryException(replace(err.var, path => "$(label)!__metadata__")))
            end
            schema = query(_SCHEMA_SQL, [path])
            stored = String[schema.name[i] for i in _top_level_rows(schema)]
            sql = _manifest_sql(_manifest_columns(stored, columns), location)
            query(sql, location ? [base, path] : [path])
        end
    end
end


# The manifest columns to return, without the computed location: either
# every kept column in file order, or `name`, `offset`, `size` followed by
# the requested extras. The location goes after `size` in the second case.
function _manifest_columns(stored::Vector{String}, columns)
    kept = [c for c in stored if !(lowercase(c) in _DROPPED_MANIFEST_COLUMNS)]
    missing_cols = [c for c in _REQUIRED_METADATA_COLUMNS if !(c in kept)]
    isempty(missing_cols) ||
        throw(ArgumentError(
            "cozip: the manifest lacks required column(s) $missing_cols"
        ))
    _check_column_names(kept, "the manifest"; reader = "decodes the manifest")
    columns === nothing && return (head = kept, extras = String[])

    wanted = unique(String[String(c) for c in columns
                           if !(lowercase(c) in _DROPPED_MANIFEST_COLUMNS)])
    unknown = [c for c in wanted if !(c in kept)]
    isempty(unknown) ||
        throw(ArgumentError(
            "cozip: unknown column(s) $unknown; the manifest has $kept"
        ))
    return (
        head   = copy(_REQUIRED_METADATA_COLUMNS),
        extras = [c for c in wanted if !(c in _REQUIRED_METADATA_COLUMNS)],
    )
end


# Columns are named explicitly. Hive partitioning stays off so the
# manifest's path never adds columns. With a location, the first parameter
# is the GDAL base and the second the manifest path.
function _manifest_sql(selected, location::Bool)::String
    items = map(_sql_identifier, selected.head)
    if location
        push!(items, "'/vsisubfile/' || \"offset\" || '_' || \"size\" || ',' || ? " *
                     "AS \"cozip:location\"")
    end
    append!(items, map(_sql_identifier, selected.extras))
    return string("SELECT ", join(items, ", "),
                  " FROM read_parquet(?, hive_partitioning = false)")
end
