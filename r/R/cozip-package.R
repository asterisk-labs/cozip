#' cozip: Cloud-Optimized ZIP
#'
#' R bindings to libcozip. A cozip archive is a regular ZIP whose
#' first entry is a binary index at byte 0, letting a reader locate
#' any priority file in one range request without scanning the
#' Central Directory.
#'
#' Three layers, mirroring the Python binding:
#' - [create()] — all-in-one writer for the FLAT profile.
#' - [stage_metadata()] — pure planner; returns the canonical
#'   metadata table.
#' - [stage_create()] — packs an archive from source files and a
#'   user-written parquet, embedded verbatim.
#'
#' For NONE / TACO profiles or buffer sources, call
#' `.Call(C_R_cozip_finalize, ...)` directly.
#'
#' @keywords internal
#' @useDynLib cozip, .registration = TRUE, .fixes = "C_"
#' @importFrom bit64 as.integer64
"_PACKAGE"