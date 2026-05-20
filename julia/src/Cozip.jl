module Cozip

include("LibCozip.jl")
include("Writer.jl")
include("Reader.jl")

const write = create

export stage_metadata, stage_create, create

end # module