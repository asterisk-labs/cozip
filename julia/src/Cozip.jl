module Cozip

include("LibCozip.jl")
include("Writer.jl")
include("Reader.jl")

const write = create

# Shared DuckDB connection. Closed at exit, before DLLs unload.
function __init__()
    _init_duckdb!()
    atexit(_close_duckdb!)
end

export stage_metadata, stage_create, create

end # module