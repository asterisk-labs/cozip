module LibCozip

using Artifacts
using LazyArtifacts
using Libdl

# Must match cozip.h.
const COZIP_SOURCE_PATH        = Cint(1)
const COZIP_ERROR_MESSAGE_SIZE = 192

mutable struct cozip_error_t
    code::Cint
    message::NTuple{COZIP_ERROR_MESSAGE_SIZE,Cchar}
    cozip_error_t() = new(Cint(0), ntuple(_ -> Cchar(0), COZIP_ERROR_MESSAGE_SIZE))
end

# isbits, so Vector{cozip_entry_t} is contiguous.
struct cozip_entry_t
    arc_name::Ptr{Cchar}
    payload_size::UInt64
    in_index::Bool
    _pad1::NTuple{7,UInt8}
    source_kind::Cint
    _source_pad::Cint
    source_path_or_data::Ptr{Cvoid}
    source_size::Csize_t
    lfh_offset::UInt64
    lfh_size::UInt64
    payload_offset::UInt64
end

function entry_from_path(arc_name::Ptr{Cchar}, payload_size::Integer,
                         in_index::Bool, path::Ptr{Cvoid})
    cozip_entry_t(
        arc_name, UInt64(payload_size), in_index, ntuple(_ -> UInt8(0), 7),
        COZIP_SOURCE_PATH, Cint(0), path, Csize_t(0),
        UInt64(0), UInt64(0), UInt64(0),
    )
end

function _lib_filename()
    Sys.isapple()   && return "cozip.dylib"
    Sys.iswindows() && return "cozip.dll"
    return "cozip.so"
end

_lib_subdir() = Sys.iswindows() ? "bin" : "lib"

function _resolve_lib_path()
    env = get(ENV, "COZIP_LIB_PATH", "")
    if !isempty(env)
        isfile(env) || error("cozip: COZIP_LIB_PATH=$env does not exist")
        return env
    end

    # LazyArtifacts makes @artifact_str auto-download lazy=true artifacts on
    # first use at runtime. No Pkg.instantiate or restart-runtime needed.
    base = artifact"cozip"

    # Release tarballs wrap a libcozip-VERSION-PLAT/ folder; descend if present.
    entries = readdir(base)
    if length(entries) == 1 && isdir(joinpath(base, entries[1]))
        base = joinpath(base, entries[1])
    end

    candidate = joinpath(base, _lib_subdir(), _lib_filename())
    isfile(candidate) || error("cozip: native library not found at $candidate")
    return candidate
end

const libcozip = Ref{Ptr{Cvoid}}(C_NULL)

_symbol(name::Symbol) = Libdl.dlsym(libcozip[], name)

# The handle only resolves in __init__, so the ccalls must run there too.
const INDEX_NAME    = Ref{String}("")
const PADDING_NAME  = Ref{String}("")
const METADATA_NAME = Ref{String}("")

function __init__()
    libcozip[] = Libdl.dlopen(_resolve_lib_path())
    INDEX_NAME[]    = unsafe_string(ccall(_symbol(:cozip_index_name), Cstring, ()))
    PADDING_NAME[]  = unsafe_string(ccall(_symbol(:cozip_padding_name), Cstring, ()))
    METADATA_NAME[] = unsafe_string(ccall(_symbol(:cozip_flat_metadata_name), Cstring, ()))
end

struct CozipError <: Exception
    code::Int
    name::String
    message::String
end

Base.showerror(io::IO, e::CozipError) = print(io, "CozipError [$(e.name)] $(e.message)")

# The message is the bytes before the first NUL. Cchar is Int8 on most
# platforms, so the bytes are reinterpreted rather than converted.
function CozipError(err::cozip_error_t)
    code = Int(err.code)
    bytes = UInt8[c % UInt8 for c in err.message]
    stop = findfirst(iszero, bytes)
    msg = String(stop === nothing ? bytes : bytes[1:stop-1])
    name_ptr = ccall(_symbol(:cozip_status_string), Cstring, (Cint,), err.code)
    name = unsafe_string(name_ptr)
    return CozipError(code, name, msg)
end

function cozip_version()
    ptr = ccall(_symbol(:cozip_version_string), Cstring, ())
    return unsafe_string(ptr)
end

function cozip_status_string(status::Integer)
    ptr = ccall(_symbol(:cozip_status_string), Cstring, (Cint,), Cint(status))
    return unsafe_string(ptr)
end

cozip_index_name()         = INDEX_NAME[]
cozip_padding_name()       = PADDING_NAME[]
cozip_flat_metadata_name() = METADATA_NAME[]

# Ref(err) aliases err in place; C writes go straight through, no copy-back.
# plan_flat!  needs length(entries) == n_users + 1   (slot for __metadata__)
# write_flat! needs length(entries) == n_users + 2   (slots for __metadata__ + padding)
function plan_flat!(entries::AbstractVector{cozip_entry_t},
                    n_users::Integer,
                    err::cozip_error_t)
    err_ref = Ref(err)
    GC.@preserve entries err_ref begin
        status = ccall(_symbol(:cozip_plan_flat), Cint,
                       (Ptr{cozip_entry_t}, Csize_t, Ptr{cozip_error_t}),
                       pointer(entries), Csize_t(n_users), err_ref)
    end
    status == 0 || throw(CozipError(err))
    return nothing
end

function write_flat!(out_path::AbstractString,
                     entries::AbstractVector{cozip_entry_t},
                     n_users::Integer,
                     metadata_parquet::AbstractString,
                     err::cozip_error_t)
    err_ref = Ref(err)
    GC.@preserve entries err_ref begin
        status = ccall(_symbol(:cozip_write_flat), Cint,
                       (Cstring, Ptr{cozip_entry_t}, Csize_t, Csize_t,
                        Cstring, Ptr{cozip_error_t}),
                       out_path, pointer(entries),
                       Csize_t(n_users), Csize_t(length(entries)),
                       metadata_parquet, err_ref)
    end
    status == 0 || throw(CozipError(err))
    return nothing
end

# Reader errors arrive as heap strings rather than in cozip_error_t.
function _reader_error(status::Cint, message::Ptr{Cchar})
    name = cozip_status_string(status)
    text = message == C_NULL ? name : unsafe_string(message)
    return CozipError(Int(status), name, text)
end

_free(ptr::Ptr) = ccall(_symbol(:cozip_free), Cvoid, (Ptr{Cvoid},), ptr)

# The __metadata__ Parquet bytes of a FLAT archive, after the index checks.
function read_flat_metadata(source::AbstractString)::Vector{UInt8}
    data = Ref{Ptr{UInt8}}(C_NULL)
    nbytes = Ref{Csize_t}(0)
    message = Ref{Ptr{Cchar}}(C_NULL)
    status = ccall(_symbol(:cozip_read_flat_metadata), Cint,
                   (Cstring, Ref{Ptr{UInt8}}, Ref{Csize_t}, Ref{Ptr{Cchar}}),
                   source, data, nbytes, message)
    try
        status == 0 || throw(_reader_error(status, message[]))
        return copy(unsafe_wrap(Vector{UInt8}, data[], Int(nbytes[])))
    finally
        _free(data[])
        _free(message[])
    end
end

# Verifies the byte-0 index and the integrity hash of an archive of any
# profile; a broken archive throws the reader's CozipError.
function verify_archive(source::AbstractString)::Nothing
    message = Ref{Ptr{Cchar}}(C_NULL)
    status = ccall(_symbol(:cozip_verify), Cint,
                   (Cstring, Ref{Ptr{Cchar}}), source, message)
    try
        status == 0 || throw(_reader_error(status, message[]))
    finally
        _free(message[])
    end
    return nothing
end

# The GDAL path that cozip:location appends after "/vsisubfile/<offset>_<size>,".
function location_base(source::AbstractString)::String
    base = Ref{Ptr{Cchar}}(C_NULL)
    message = Ref{Ptr{Cchar}}(C_NULL)
    status = ccall(_symbol(:cozip_location_base), Cint,
                   (Cstring, Ref{Ptr{Cchar}}, Ref{Ptr{Cchar}}),
                   source, base, message)
    try
        status == 0 || throw(_reader_error(status, message[]))
        return unsafe_string(base[])
    finally
        _free(base[])
        _free(message[])
    end
end

end # module
