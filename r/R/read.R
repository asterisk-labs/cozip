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

  enabled <- if (location) "true" else "false"
  sql <- sprintf("SELECT %s FROM read_flat(?, location := %s)",
                 .build_select(columns, location), enabled)
  legacy <- FALSE
  result <- tryCatch(
    DBI::dbGetQuery(con, sql, params = list(source)),
    error = function(err) {
      message <- conditionMessage(err)
      if (.legacy_flat_signature(message)) {
        legacy_sql <- sprintf(
          "SELECT %s FROM read_flat(?, gdal_vsi := %s)",
          .build_select(columns, location, .LEGACY_LOCATION_COLUMN),
          enabled
        )
      } else if (.missing_read_flat(message)) {
        legacy_sql <- sprintf(
          "SELECT %s FROM read_cozip(?, gdal_vsi := %s)",
          .build_select(columns, location, .LEGACY_LOCATION_COLUMN),
          enabled
        )
      } else {
        stop(err)
      }
      legacy <<- TRUE
      DBI::dbGetQuery(con, legacy_sql, params = list(source))
    }
  )
  result <- tibble::as_tibble(result)
  protected <- intersect(.PROTECTED_LOCATION_COLUMNS, names(result))
  if (legacy && location) {
    result <- result[setdiff(names(result), protected)]
  } else if ("taco:location" %in% protected) {
    result <- result[setdiff(names(result), "taco:location")]
  }
  if (legacy && location && .LEGACY_LOCATION_COLUMN %in% names(result)) {
    names(result)[names(result) == .LEGACY_LOCATION_COLUMN] <- .LOCATION_COLUMN
  }
  if (!location) {
    result <- result[setdiff(
      names(result), c(.LOCATION_COLUMN, .LEGACY_LOCATION_COLUMN)
    )]
  }
  result
}


.quote_ident <- function(s) sprintf('"%s"', gsub('"', '""', s, fixed = TRUE))


.LOCATION_COLUMN <- "cozip:location"
.LEGACY_LOCATION_COLUMN <- "cozip:gdal_vsi"


.build_select <- function(columns, location,
                          source_location_column = .LOCATION_COLUMN) {
  if (is.null(columns)) {
    return("*")
  }
  required <- c("name", "offset", "size")
  if (location) required <- c(required, .LOCATION_COLUMN)
  extras <- setdiff(columns, .PROTECTED_LOCATION_COLUMNS)
  ordered <- c(required, setdiff(extras, required))
  expressions <- vapply(ordered, function(column) {
    source <- if (identical(column, .LOCATION_COLUMN)) {
      source_location_column
    } else {
      column
    }
    expression <- .quote_ident(source)
    if (!identical(source, column)) {
      expression <- paste(expression, "AS", .quote_ident(column))
    }
    expression
  }, character(1))
  paste(expressions, collapse = ", ")
}


.legacy_flat_signature <- function(message) {
  grepl("read_flat", message, fixed = TRUE) &&
    grepl("gdal_vsi", message, fixed = TRUE) &&
    grepl("does not support the supplied arguments", message, fixed = TRUE)
}


.missing_read_flat <- function(message) {
  grepl("read_flat", message, fixed = TRUE) &&
    grepl("does not exist", message, fixed = TRUE)
}
