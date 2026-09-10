using DataFrames
using DuckDB


# Forward declaration: claims `read` as a name owned by Cozip
function read end


# Loaded lazily on first Reader.read so `using Cozip` stays offline.
const _DUCKDB_EXTS_LOADED = Ref(false)
const _LOCATION_COLUMN = "cozip:location"
const _LEGACY_LOCATION_COLUMN = "cozip:gdal_vsi"


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
        legacy = false
        query = try
            DBInterface.execute(con, sql, [src])
        catch err
            message = sprint(showerror, err)
            enabled = location ? "true" : "false"
            legacy_sql = if _legacy_flat_signature(message)
                string(
                    "SELECT ", _build_select(columns, location, _LEGACY_LOCATION_COLUMN),
                    " FROM read_flat(?, gdal_vsi := ", enabled, ")",
                )
            elseif _missing_read_flat(message)
                string(
                    "SELECT ", _build_select(columns, location, _LEGACY_LOCATION_COLUMN),
                    " FROM read_cozip(?, gdal_vsi := ", enabled, ")",
                )
            else
                rethrow()
            end
            legacy = true
            DBInterface.execute(con, legacy_sql, [src])
        end
        result = DataFrame(query)
        protected = intersect(collect(_PROTECTED_LOCATION_COLUMNS), names(result))
        if legacy && location
            isempty(protected) || select!(result, Not(Symbol.(protected)))
        elseif "taco:location" in protected
            select!(result, Not(Symbol("taco:location")))
        end
        if legacy && location && _LEGACY_LOCATION_COLUMN in names(result)
            rename!(result, Symbol(_LEGACY_LOCATION_COLUMN) => Symbol(_LOCATION_COLUMN))
        end
        if !location
            drop = intersect([_LOCATION_COLUMN, _LEGACY_LOCATION_COLUMN], names(result))
            isempty(drop) || select!(result, Not(Symbol.(drop)))
        end
        result
    end
end


_quote_ident(s) = "\"" * replace(String(s), "\"" => "\"\"") * "\""


function _build_select(columns, location, source_location_column=_LOCATION_COLUMN)
    columns === nothing && return "*"
    required = ["name", "offset", "size"]
    location && push!(required, _LOCATION_COLUMN)
    extras = String.(columns)
    filter!(c -> !(c in _PROTECTED_LOCATION_COLUMNS), extras)
    ordered = vcat(required, setdiff(extras, required))
    expressions = map(ordered) do column
        source = column == _LOCATION_COLUMN ? source_location_column : column
        expression = _quote_ident(source)
        source == column ? expression : expression * " AS " * _quote_ident(column)
    end
    join(expressions, ", ")
end


_legacy_flat_signature(message) =
    occursin("read_flat", message) &&
    occursin("gdal_vsi", message) &&
    occursin("does not support the supplied arguments", message)


_missing_read_flat(message) =
    occursin("read_flat", message) && occursin("does not exist", message)
