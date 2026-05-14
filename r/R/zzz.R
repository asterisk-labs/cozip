# Internal constants and shared helpers.

# Profile selectors. Match cozip_profile_t in cozip.h.
.PROFILE_NONE <- 0L
.PROFILE_FLAT <- 1L
.PROFILE_TACO <- 2L

# Reserved entry names. Pulled from libcozip in .onLoad (the DLL
# isn't loaded yet when this file is sourced into the lazy DB).
# The placeholders here get overwritten before any user code runs.
.RESERVED       <- NULL
.RESERVED_NAMES <- NULL
.META_NAME      <- NULL

# Columns the binding computes; rejected in stage_metadata() input.
.RESERVED_INPUT_COLUMNS <- c("offset", "size")

# Columns required in the metadata parquet for stage_create().
.REQUIRED_METADATA_COLUMNS <- c("name", "offset", "size")


# Error helper. Mirrors `raise ValueError("cozip: ...")` in _writer.py.
.cozip_stop <- function(fmt, ...) {
  stop(sprintf(paste0("cozip: ", fmt), ...), call. = FALSE)
}


# Pull reserved names from libcozip and run sanity checks. Runs
# after useDynLib loads the DLL but before user code sees the
# package, so callers always see the populated constants.
.onLoad <- function(libname, pkgname) {
  reserved <- .Call(C_R_cozip_reserved_names)

  .RESERVED       <<- reserved
  .RESERVED_NAMES <<- unname(reserved)
  .META_NAME      <<- reserved[["flat_metadata"]]

  stopifnot(
    .PROFILE_NONE == 0L,
    .PROFILE_FLAT == 1L,
    .PROFILE_TACO == 2L,
    identical(names(.RESERVED), c("index", "padding", "flat_metadata")),
    !anyDuplicated(.RESERVED_NAMES),
    nzchar(.META_NAME)
  )
}