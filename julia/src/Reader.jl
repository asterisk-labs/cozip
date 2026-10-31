using DataFrames
using DuckDB

using .LibCozip: read_flat_metadata, location_base


# Forward declaration: claims `read` as a name owned by Cozip
function read end


const _LOCATION_COLUMN = "cozip:location"


# The body of the read_flat macro in cozip_reader, over the fetched manifest.
# Hive partitioning stays off so the manifest's path never adds columns.
_manifest_sql(location::Bool) = string(
    "SELECT COLUMNS(lambda c: c != 'cozip:location' AND c != 'cozip:gdal_vsi'), ",
    "CASE WHEN ", location ? "true" : "false", " ",
    "THEN '/vsisubfile/' || \"offset\" || '_' || \"size\" || ',' || ? ",
    "ELSE NULL END AS \"cozip:location\" ",
    "FROM read_parquet(?, hive_partitioning = false)",
)


"""
    read(source; columns=nothing, location=true) -> DataFrame

Read the manifest of a FLAT-profile cozip archive. The byte-0 index is
read natively and only the `__metadata__` Parquet manifest is fetched;
DuckDB decodes it without loading any extension. `name`, `offset`,
`size` (and `cozip:location` when `location=true`) are always included;
pass `columns` to bring extras, `nothing` brings all.

Archives using any profile other than Flat (`profile = 1`), including TACO
(`profile = 2`), are rejected with a `CozipError`.

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
        cozip: Cozip.read is not supported on Windows yet.

        The native reader has not been verified on Windows. Writer
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

    metadata = read_flat_metadata(src)
    base = location ? location_base(src) : ""

    return mktempdir(prefix="cozip-") do dir
        path = joinpath(dir, "__metadata__.parquet")
        Base.write(path, metadata)
        sql = string(
            "SELECT ", _build_select(columns, location),
            " FROM (", _manifest_sql(location), ")",
        )
        con = _duckdb_con()
        result = lock(_DUCKDB_LOCK) do
            try
                DataFrame(DBInterface.execute(con, sql, [base, path]))
            catch err
                (err isa DuckDB.QueryException && occursin(path, err.var)) || rethrow()
                label = occursin("://", src) ? "remote archive" : src
                throw(DuckDB.QueryException(replace(err.var, path => "$(label)!__metadata__")))
            end
        end
        if "taco:location" in names(result)
            select!(result, Not(Symbol("taco:location")))
        end
        if !location && _LOCATION_COLUMN in names(result)
            select!(result, Not(Symbol(_LOCATION_COLUMN)))
        end
        result
    end
end


_quote_ident(s) = "\"" * replace(String(s), "\"" => "\"\"") * "\""


function _build_select(columns, location)
    columns === nothing && return "*"
    required = ["name", "offset", "size"]
    location && push!(required, _LOCATION_COLUMN)
    extras = String.(columns)
    filter!(c -> !(c in _PROTECTED_LOCATION_COLUMNS), extras)
    ordered = vcat(required, setdiff(extras, required))
    join(map(_quote_ident, ordered), ", ")
end
