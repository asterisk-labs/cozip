using DataFrames
using DuckDB


# Forward declaration: claims `read` as a name owned by Cozip, so
# `Cozip.read` is independent of `Base.read` instead of extending it.
function read end


"""
    read(source; columns=nothing, gdal_vsi=true) -> DataFrame

Read the manifest of a FLAT-profile cozip archive via the DuckDB
cozip extension. `name`, `offset`, `size` (and `cozip:gdal_vsi` when
`gdal_vsi=true`) are always included; pass `columns` to bring
extras, `nothing` brings all.

# Arguments
- `source`: local path or http(s)/s3/gcs/azure/hf URL to the `.zip`.
- `columns`: vector of extra column names. `nothing` returns every
  column.
- `gdal_vsi`: include the `cozip:gdal_vsi` column.
"""
function read(
    source::AbstractString;
    columns::Union{Nothing,AbstractVector{<:AbstractString}} = nothing,
    gdal_vsi::Bool = true,
)::DataFrame
    isempty(source) &&
        throw(ArgumentError("cozip: `source` must be a non-empty string"))

    if columns !== nothing && any(isempty, columns)
        throw(ArgumentError("cozip: `columns` entries must be non-empty"))
    end

    db  = DuckDB.DB()
    con = DBInterface.connect(db)
    try
        DBInterface.execute(con, "INSTALL httpfs")
        DBInterface.execute(con, "LOAD httpfs")
        DBInterface.execute(con, "INSTALL cozip FROM community")
        DBInterface.execute(con, "LOAD cozip")

        sql = string(
            "SELECT ", _build_select(columns, gdal_vsi),
            " FROM read_cozip(?, gdal_vsi := ", gdal_vsi ? "true" : "false", ")",
        )
        return DataFrame(DBInterface.execute(con, sql, [String(source)]))
    finally
        DBInterface.close!(con)
        DBInterface.close!(db)
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