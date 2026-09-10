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
    local_extension = get(ENV, "COZIP_EXTENSION", "")
    if isempty(local_extension)
        DBInterface.execute(con, "INSTALL cozip FROM community")
        DBInterface.execute(con, "LOAD cozip")
    else
        quoted = replace(local_extension, "'" => "''")
        DBInterface.execute(con, "LOAD '$(quoted)'")
    end
    _DUCKDB_EXTS_LOADED[] = true
    return nothing
end


"""
    read(source; columns=nothing, location=true) -> DataFrame

Read the manifest of a FLAT-profile cozip archive via the DuckDB
cozip extension. `name`, `offset`, `size` (and `cozip:location` when
`location=true`) are always included; pass `columns` to bring
extras, `nothing` brings all.

Archives using any profile other than Flat (`profile = 1`), including TACO
(`profile = 2`), are rejected by `read_flat`.

Not supported on Windows yet. The writer is.

# Arguments
- `source`: local path or http(s)/s3/gcs/azure/hf URL to the `.zip`.
- `columns`: vector of extra column names. `nothing` returns every
  column.
- `location`: include the `cozip:location` column.
"""
function read(
    source;
    columns::Union{Nothing,AbstractVector{<:AbstractString}} = nothing,
    location::Bool = true,
)::DataFrame
    if Sys.iswindows()
        error("""
        cozip: Cozip.read is not supported on Windows.

        DuckDB.jl on Windows crashes when loading community extensions
        that register a filesystem, which read_flat requires. Writer
        operations (Cozip.create, stage_metadata, stage_create) work
        fine. For reads, use Linux, macOS, WSL, or the Python binding.
        """)
    end

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

    if columns !== nothing && any(isempty, columns)
        throw(ArgumentError("cozip: `columns` entries must be non-empty"))
    end

    con = _duckdb_con()

    return lock(_DUCKDB_LOCK) do
        _ensure_duckdb_extensions!(con)

        sql = string(
            "SELECT ", _build_select(columns, location),
            " FROM read_flat(?, location := ", location ? "true" : "false", ")",
        )
        query = try
            DBInterface.execute(con, sql, [src])
        catch err
            message = sprint(showerror, err)
            if !occursin("read_flat", message) || !occursin("does not exist", message)
                rethrow()
            end
            legacy_sql = replace(sql, "read_flat(" => "read_cozip("; count=1)
            DBInterface.execute(con, legacy_sql, [src])
        end
        result = DataFrame(query)
        if !location && "cozip:location" in names(result)
            select!(result, Not(Symbol("cozip:location")))
        end
        result
    end
end


_quote_ident(s) = "\"" * replace(String(s), "\"" => "\"\"") * "\""


function _build_select(columns, location)
    columns === nothing && return "*"
    required = ["name", "offset", "size"]
    location && push!(required, "cozip:location")
    extras = String.(columns)
    location || filter!(c -> c != "cozip:location", extras)
    ordered = vcat(required, setdiff(extras, required))
    join((_quote_ident(c) for c in ordered), ", ")
end
