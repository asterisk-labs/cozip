using DataFrames
using DuckDB


# Forward declaration: claims `read` as a name owned by Cozip
function read end


# Loaded lazily on first Reader.read so `using Cozip` stays offline.
const _DUCKDB_EXTS_LOADED = Ref(false)


# Caller already holds _DUCKDB_LOCK.
function _ensure_duckdb_extensions!(con)
    _DUCKDB_EXTS_LOADED[] && return nothing
    DBInterface.execute(con, "INSTALL httpfs")
    DBInterface.execute(con, "LOAD httpfs")
    DBInterface.execute(con, "INSTALL cozip FROM community")
    DBInterface.execute(con, "LOAD cozip")
    _DUCKDB_EXTS_LOADED[] = true
    return nothing
end


"""
    read(source; columns=nothing, gdal_vsi=true) -> DataFrame

Read the manifest of a FLAT-profile cozip archive via the DuckDB
cozip extension. `name`, `offset`, `size` (and `cozip:gdal_vsi` when
`gdal_vsi=true`) are always included; pass `columns` to bring
extras, `nothing` brings all.

Not supported on Windows yet. The writer is.

# Arguments
- `source`: local path or http(s)/s3/gcs/azure/hf URL to the `.zip`.
- `columns`: vector of extra column names. `nothing` returns every
  column.
- `gdal_vsi`: include the `cozip:gdal_vsi` column.
"""
function read(
    source;
    columns::Union{Nothing,AbstractVector{<:AbstractString}} = nothing,
    gdal_vsi::Bool = true,
)::DataFrame
    if Sys.iswindows()
        error("""
        cozip: Cozip.read is not supported on Windows.

        DuckDB.jl on Windows crashes when loading community extensions
        that register a filesystem, which read_cozip requires. Writer
        operations (Cozip.create, stage_metadata, stage_create) work
        fine. For reads, use Linux, macOS, WSL, or the Python binding.
        """)
    end

    src = string(source)
    isempty(src) &&
        throw(ArgumentError("cozip: `source` must be a non-empty string"))

    if columns !== nothing && any(isempty, columns)
        throw(ArgumentError("cozip: `columns` entries must be non-empty"))
    end

    con = _duckdb_con()

    return lock(_DUCKDB_LOCK) do
        _ensure_duckdb_extensions!(con)

        sql = string(
            "SELECT ", _build_select(columns, gdal_vsi),
            " FROM read_cozip(?, gdal_vsi := ", gdal_vsi ? "true" : "false", ")",
        )
        DataFrame(DBInterface.execute(con, sql, [src]))
    end
end


_quote_ident(s) = "\"" * replace(String(s), "\"" => "\"\"") * "\""


function _build_select(columns, gdal_vsi)
    columns === nothing && return "*"
    required = ["name", "offset", "size"]
    gdal_vsi && push!(required, "cozip:gdal_vsi")
    ordered = vcat(required, setdiff(String.(columns), required))
    join((_quote_ident(c) for c in ordered), ", ")
end