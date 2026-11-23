# Internal constants and shared helpers.

# Profile selector. Matches COZIP_PROFILE_FLAT in cozip.h.
.PROFILE_FLAT <- 1L

# Reserved entry names. Pulled from libcozip in .onLoad (the DLL
# isn't loaded yet when this file is sourced into the lazy DB).
# The placeholders here get overwritten before any user code runs.
.RESERVED       <- NULL
.RESERVED_NAMES <- NULL
.META_NAME      <- NULL

# Columns owned by the writer or reader; producers must never persist them.
# DuckDB compares identifiers without case, so the checks fold case too.
.PROTECTED_LOCATION_COLUMNS <- c("cozip:location", "taco:location")
.RESERVED_INPUT_COLUMNS <- c("offset", "size", .PROTECTED_LOCATION_COLUMNS)

# Columns required in the metadata parquet for stage_create().
.REQUIRED_METADATA_COLUMNS <- c("name", "offset", "size")

# Stored columns read() never returns, compared without case: the
# reader-owned location names (spec 12.3) and the former location column.
.DROPPED_MANIFEST_COLUMNS <- c(.PROTECTED_LOCATION_COLUMNS, "cozip:gdal_vsi")


# Error helper. Mirrors `raise ValueError("cozip: ...")` in _writer.py.
.cozip_stop <- function(fmt, ...) {
  stop(sprintf(paste0("cozip: ", fmt), ...), call. = FALSE)
}


# The first two names DuckDB would treat as one identifier, or NULL.
.case_pair <- function(cols) {
  lower <- tolower(cols)
  i <- which(duplicated(lower))
  if (length(i) == 0L) return(NULL)
  c(cols[match(lower[i[1L]], lower)], cols[i[1L]])
}


# Column names must stay distinct when compared without case.
.check_column_names <- function(cols, what, reader) {
  pair <- .case_pair(cols)
  if (is.null(pair)) return(invisible(NULL))
  if (identical(pair[1L], pair[2L])) {
    .cozip_stop("%s has the column %s more than once", what, shQuote(pair[1L]))
  }
  .cozip_stop(
    "%s has columns that differ only in case: %s and %s; DuckDB, which %s, cannot tell them apart",
    what, shQuote(pair[1L]), shQuote(pair[2L]), reader
  )
}


.quoted_list <- function(x) paste(shQuote(x), collapse = ", ")


# Pull reserved names from libcozip and run sanity checks. Runs
# after useDynLib loads the DLL but before user code sees the
# package, so callers always see the populated constants.
.onLoad <- function(libname, pkgname) {
  reserved <- .Call(C_R_cozip_reserved_names)

  .RESERVED       <<- reserved
  .RESERVED_NAMES <<- unname(reserved)
  .META_NAME      <<- reserved[["flat_metadata"]]

  stopifnot(
    .PROFILE_FLAT == 1L,
    identical(names(.RESERVED), c("index", "padding", "flat_metadata")),
    !anyDuplicated(.RESERVED_NAMES),
    nzchar(.META_NAME)
  )
}
