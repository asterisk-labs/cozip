#' Read the manifest of a FLAT-profile cozip archive
#'
#' Thin wrapper around the DuckDB cozip extension. `name`, `offset`,
#' `size` (and `cozip:gdal_vsi` when `gdal_vsi = TRUE`) are always
#' included; use `columns` to bring extras, `NULL` brings all.
#'
#' @param source Local path or http(s)/s3/gcs/azure/hf URL to the `.zip`.
#' @param columns Character vector of extra columns. `NULL` returns
#'   every column.
#' @param gdal_vsi Include the `cozip:gdal_vsi` column.
#'
#' @return A tibble.
#' @export
read <- function(source, columns = NULL, gdal_vsi = TRUE) {
  if (!is.character(source) || length(source) != 1L || is.na(source)) {
    .cozip_stop("`source` must be a single non-NA string")
  }
  if (!is.null(columns) && (!is.character(columns) || anyNA(columns))) {
    .cozip_stop("`columns` must be a character vector with no NAs (or NULL)")
  }
  if (!is.logical(gdal_vsi) || length(gdal_vsi) != 1L || is.na(gdal_vsi)) {
    .cozip_stop("`gdal_vsi` must be TRUE or FALSE")
  }

  con <- duckdb::dbConnect(duckdb::duckdb())
  on.exit(duckdb::dbDisconnect(con, shutdown = TRUE), add = TRUE)

  DBI::dbExecute(con, "INSTALL httpfs")
  DBI::dbExecute(con, "LOAD httpfs")
  DBI::dbExecute(con, "INSTALL cozip FROM community")
  DBI::dbExecute(con, "LOAD cozip")

  sql <- sprintf(
    "SELECT %s FROM read_cozip(?, gdal_vsi := %s)",
    .build_select(columns, gdal_vsi),
    if (gdal_vsi) "true" else "false"
  )
  tibble::as_tibble(DBI::dbGetQuery(con, sql, params = list(source)))
}


.quote_ident <- function(s) sprintf('"%s"', gsub('"', '""', s, fixed = TRUE))


.build_select <- function(columns, gdal_vsi) {
  if (is.null(columns)) return("*")
  required <- c("name", "offset", "size")
  if (gdal_vsi) required <- c(required, "cozip:gdal_vsi")
  ordered <- c(required, setdiff(columns, required))
  paste(vapply(ordered, .quote_ident, character(1)), collapse = ", ")
}