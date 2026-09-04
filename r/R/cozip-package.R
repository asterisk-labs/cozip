#' cozip: Cloud-Optimized ZIP
#'
#' R bindings to libcozip. A cozip archive is a regular ZIP whose
#' first entry is a binary index at byte 0, letting a reader locate
#' any priority file in one range request without scanning the
#' Central Directory.
#'
#' The writer has three public functions:
#' - [create()] — all-in-one writer for the FLAT profile.
#' - [stage_metadata()] — plans offsets after checking the source files.
#' - [stage_create()] — packs an archive from source files and a
#'   user-written parquet, embedded verbatim.
#'
#' @keywords internal
#' @useDynLib cozip, .registration = TRUE, .fixes = "C_"
#' @importFrom bit64 as.integer64
"_PACKAGE"
