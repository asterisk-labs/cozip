#' Read the manifest of a FLAT-profile cozip archive
#'
#' Thin wrapper around the DuckDB cozip extension. `name`, `offset`,
#' `size` (and `cozip:gdal_vsi` when `gdal_vsi = TRUE`) are always
#' included; use `columns` to bring extras, `NULL` brings all.
#' Archives using any profile other than Flat (`profile = 1`), including TACO
#' (`profile = 2`), are rejected by `read_flat()`.
#'
#' @param source Local path or http(s)/s3/gcs/azure/hf URL to the `.zip`.
#' @param columns Character vector of extra columns. `NULL` returns
#'   every column.
#' @param gdal_vsi Include the `cozip:gdal_vsi` column.
#'
#' @return A tibble.
#' @export
read <- function(source, columns = NULL, gdal_vsi = TRUE) {
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
  if (!is.logical(gdal_vsi) || length(gdal_vsi) != 1L || is.na(gdal_vsi)) {
    .cozip_stop("`gdal_vsi` must be TRUE or FALSE")
  }

  local_extension <- Sys.getenv("COZIP_EXTENSION", unset = "")
  driver <- if (nzchar(local_extension)) {
    duckdb::duckdb(config = list(allow_unsigned_extensions = "true"))
  } else {
    duckdb::duckdb()
  }
  con <- duckdb::dbConnect(driver)
  on.exit(duckdb::dbDisconnect(con, shutdown = TRUE), add = TRUE)

  DBI::dbExecute(con, "INSTALL httpfs")
  DBI::dbExecute(con, "LOAD httpfs")
  if (nzchar(local_extension)) {
    DBI::dbExecute(
      con,
      paste("LOAD", DBI::dbQuoteString(con, local_extension))
    )
  } else {
    DBI::dbExecute(con, "INSTALL cozip FROM community")
    DBI::dbExecute(con, "LOAD cozip")
  }

  sql <- sprintf(
    "SELECT %s FROM read_flat(?, gdal_vsi := %s)",
    .build_select(columns, gdal_vsi),
    if (gdal_vsi) "true" else "false"
  )
  result <- tryCatch(
    DBI::dbGetQuery(con, sql, params = list(source)),
    error = function(err) {
      message <- conditionMessage(err)
      if (!grepl("read_flat", message, fixed = TRUE)
          || !grepl("does not exist", message, fixed = TRUE)) {
        stop(err)
      }
      legacy_sql <- sub("FROM read_flat\\(", "FROM read_cozip(", sql)
      DBI::dbGetQuery(con, legacy_sql, params = list(source))
    }
  )
  result <- tibble::as_tibble(result)
  if (!gdal_vsi && "cozip:gdal_vsi" %in% names(result)) {
    result <- result[setdiff(names(result), "cozip:gdal_vsi")]
  }
  result
}


.quote_ident <- function(s) sprintf('"%s"', gsub('"', '""', s, fixed = TRUE))


.build_select <- function(columns, gdal_vsi) {
  if (is.null(columns)) {
    return("*")
  }
  required <- c("name", "offset", "size")
  if (gdal_vsi) required <- c(required, "cozip:gdal_vsi")
  extras <- columns
  if (!gdal_vsi) extras <- setdiff(extras, "cozip:gdal_vsi")
  ordered <- c(required, setdiff(extras, required))
  paste(vapply(ordered, .quote_ident, character(1)), collapse = ", ")
}
