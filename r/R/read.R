#' Read the manifest of a FLAT-profile cozip archive
#'
#' Thin wrapper around the DuckDB cozip extension. `name`, `offset`,
#' `size` (and `cozip:location` when `location = TRUE`) are always
#' included; use `columns` to bring extras, `NULL` brings all.
#' Archives using any profile other than Flat (`profile = 1`), including TACO
#' (`profile = 2`), are rejected by `read_flat()`.
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
    "SELECT %s FROM read_flat(?, location := %s)",
    .build_select(columns, location),
    if (location) "true" else "false"
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
  if (!location && "cozip:location" %in% names(result)) {
    result <- result[setdiff(names(result), "cozip:location")]
  }
  result
}


.quote_ident <- function(s) sprintf('"%s"', gsub('"', '""', s, fixed = TRUE))


.build_select <- function(columns, location) {
  if (is.null(columns)) {
    return("*")
  }
  required <- c("name", "offset", "size")
  if (location) required <- c(required, "cozip:location")
  extras <- columns
  if (!location) extras <- setdiff(extras, "cozip:location")
  ordered <- c(required, setdiff(extras, required))
  paste(vapply(ordered, .quote_ident, character(1)), collapse = ", ")
}
