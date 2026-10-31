#' Read the manifest of a FLAT-profile cozip archive
#'
#' Reads the byte-0 index natively, fetches only the `__metadata__`
#' Parquet manifest, and decodes it with DuckDB, which loads no
#' extension. `name`, `offset`, `size` (and `cozip:location` when
#' `location = TRUE`) are always included; use `columns` to bring
#' extras, `NULL` brings all.
#' Archives using any profile other than Flat (`profile = 1`), including TACO
#' (`profile = 2`), are rejected.
#'
#' @param source Local path or http(s)/s3/gcs/azure/hf URL to the `.zip`.
#' @param columns Character vector of extra columns. `NULL` returns
#'   every column.
#' @param location Include the `cozip:location` column.
#'
#' @return A tibble.
#' @export
read <- function(source, columns = NULL, location = TRUE) {
  if (!is.character(source) || length(source) != 1L || is.na(source)
      || !nzchar(source)) {
    .cozip_stop("`source` must be a single non-empty string")
  }
  if (any(as.integer(charToRaw(enc2utf8(source))) >= 128L)) {
    .cozip_stop(
      "`source` contains non-ASCII characters; cozip supports ASCII paths and URLs only"
    )
  }
  if (!is.null(columns) && (!is.character(columns) || anyNA(columns))) {
    .cozip_stop("`columns` must be a character vector with no NAs (or NULL)")
  }
  if (!is.null(columns) && any(!nzchar(columns))) {
    .cozip_stop("`columns` entries must be non-empty")
  }
  if (!is.logical(location) || length(location) != 1L || is.na(location)) {
    .cozip_stop("`location` must be TRUE or FALSE")
  }

  metadata <- tempfile("cozip-", fileext = ".parquet")
  on.exit(unlink(metadata), add = TRUE)
  writeBin(.Call(C_R_cozip_read_flat_metadata, source), metadata)
  base <- if (location) .Call(C_R_cozip_location_base, source) else ""

  con <- duckdb::dbConnect(.manifest_driver())
  # Disconnect before the temporary manifest is removed.
  on.exit(duckdb::dbDisconnect(con, shutdown = TRUE), add = TRUE, after = FALSE)

  sql <- sprintf("SELECT %s FROM (%s)",
                 .build_select(columns, location),
                 sprintf(.MANIFEST_SQL, if (location) "true" else "false"))
  result <- tryCatch(
    DBI::dbGetQuery(con, sql, params = list(base, metadata)),
    error = function(err) {
      text <- conditionMessage(err)
      if (!grepl(metadata, text, fixed = TRUE)) stop(err)
      label <- if (grepl("://", source, fixed = TRUE)) "remote archive" else source
      stop(gsub(metadata, paste0(label, "!__metadata__"), text, fixed = TRUE),
           call. = FALSE)
    }
  )
  result <- tibble::as_tibble(result)
  if ("taco:location" %in% names(result)) {
    result <- result[setdiff(names(result), "taco:location")]
  }
  if (!location) {
    result <- result[setdiff(names(result), .LOCATION_COLUMN)]
  }
  result
}


# DuckDB only decodes a local file, so it must never install or load an
# extension. duckdb 1.5.5 can also refuse extensions outright and stay out of
# ~/.duckdb, which the manifest read never needs.
.manifest_driver <- function() {
  args <- list(config = list(
    autoinstall_known_extensions = "false",
    autoload_known_extensions = "false"
  ))
  for (option in c("shared_home", "allow_extensions")) {
    if (option %in% names(formals(duckdb::duckdb))) args[[option]] <- FALSE
  }
  do.call(duckdb::duckdb, args)
}


.quote_ident <- function(s) sprintf('"%s"', gsub('"', '""', s, fixed = TRUE))


.LOCATION_COLUMN <- "cozip:location"


# The body of the read_flat macro in cozip_reader, over the fetched manifest.
# Hive partitioning stays off so the manifest's path never adds columns.
.MANIFEST_SQL <- paste(
  "SELECT COLUMNS(lambda c: c != 'cozip:location' AND c != 'cozip:gdal_vsi'),",
  "CASE WHEN %s",
  "THEN '/vsisubfile/' || \"offset\" || '_' || \"size\" || ',' || ?",
  "ELSE NULL END AS \"cozip:location\"",
  "FROM read_parquet(?, hive_partitioning = false)"
)


.build_select <- function(columns, location) {
  if (is.null(columns)) {
    return("*")
  }
  required <- c("name", "offset", "size")
  if (location) required <- c(required, .LOCATION_COLUMN)
  extras <- setdiff(columns, .PROTECTED_LOCATION_COLUMNS)
  ordered <- c(required, setdiff(extras, required))
  paste(vapply(ordered, .quote_ident, character(1)), collapse = ", ")
}
