#' Check a cozip archive's access path
#'
#' For any profile, checks the header, index, indexed names and ranges,
#' integrity hash and ZIP end record (spec 8.5 and 7). It does not parse
#' the Central Directory, check payloads or enforce profile rules.
#'
#' @param source Local path or http(s)/s3/gcs/azure/hf URL to the `.zip`.
#'
#' @return `NULL`, invisibly, when these checks pass. A failed check stops
#'   with an error that names the rule, such as
#'   `[cozip:HASH_MISMATCH]`.
#' @export
verify <- function(source) {
  .check_source(source)
  .Call(C_R_cozip_verify, source)
  invisible(NULL)
}


.check_source <- function(source) {
  if (!is.character(source) || length(source) != 1L || is.na(source)
      || !nzchar(source)) {
    .cozip_stop("`source` must be a single non-empty string")
  }
  if (any(as.integer(charToRaw(enc2utf8(source))) >= 128L)) {
    .cozip_stop(
      "`source` contains non-ASCII characters; cozip supports ASCII paths and URLs only"
    )
  }
  invisible(NULL)
}


#' Read the manifest of a FLAT-profile cozip archive
#'
#' Reads the byte-0 index natively, fetches only the `__metadata__`
#' Parquet manifest, and decodes it with DuckDB, which loads no
#' extension. Stored `cozip:location`, `taco:location` and
#' `cozip:gdal_vsi` columns are dropped in any letter case, and
#' `cozip:location` is computed again.
#' Archives using any profile other than Flat (`profile = 1`), including TACO
#' (`profile = 2`), are rejected.
#'
#' @param source Local path or http(s)/s3/gcs/azure/hf URL to the `.zip`.
#' @param columns `NULL` returns every manifest column. Otherwise a
#'   character vector of extra columns, matched exactly; `name`, `offset`
#'   and `size` always come first, then `cozip:location` when enabled,
#'   then the extras in the order given. An unknown name is an error;
#'   location names are ignored in any case.
#' @param location Include the computed `cozip:location` column.
#'
#' @return A tibble.
#' @export
read <- function(source, columns = NULL, location = TRUE) {
  .check_source(source)
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
  base <- if (location) .Call(C_R_cozip_location_base, source) else NULL

  con <- duckdb::dbConnect(.manifest_driver())
  # Disconnect before the temporary manifest is removed.
  on.exit(duckdb::dbDisconnect(con, shutdown = TRUE), add = TRUE, after = FALSE)

  label <- if (grepl("://", source, fixed = TRUE)) "remote archive" else source
  query <- function(sql, params) {
    tryCatch(
      DBI::dbGetQuery(con, sql, params = params),
      error = function(err) {
        text <- conditionMessage(err)
        if (!grepl(metadata, text, fixed = TRUE)) stop(err)
        stop(gsub(metadata, paste0(label, "!__metadata__"), text, fixed = TRUE),
             call. = FALSE)
      }
    )
  }

  stored <- .top_level_columns(
    query("SELECT name, num_children FROM parquet_schema(?)", list(metadata))
  )
  sql <- .manifest_sql(.manifest_columns(stored, columns), location)
  params <- if (location) list(base, metadata) else list(metadata)
  tibble::as_tibble(query(sql, params))
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


# Top-level column names as stored, from the rows of DuckDB parquet_schema().
# The first row is the root; each column is followed by its nested children,
# which are skipped by counting num_children.
.top_level_columns <- function(schema) {
  children <- schema$num_children
  children[is.na(children)] <- 0
  if (length(children) == 0L) return(character(0))
  out <- character(children[1L])
  pos <- 2L
  for (i in seq_along(out)) {
    out[i] <- schema$name[pos]
    pending <- 1
    while (pending > 0) {
      if (pos > length(children)) .cozip_stop("cannot read the manifest schema")
      pending <- pending - 1 + children[pos]
      pos <- pos + 1L
    }
  }
  out
}


# The manifest columns to return, without the computed location: either
# every kept column in file order, or `name`, `offset`, `size` followed by
# the requested extras. The location goes after `size` in the second case.
.manifest_columns <- function(stored, columns) {
  kept <- stored[!tolower(stored) %in% .DROPPED_MANIFEST_COLUMNS]
  missing <- setdiff(.REQUIRED_METADATA_COLUMNS, kept)
  if (length(missing) > 0L) {
    .cozip_stop("the manifest lacks required column(s) %s", .quoted_list(missing))
  }
  .check_column_names(kept, "the manifest", "decodes the manifest")
  if (is.null(columns)) {
    return(list(head = kept, extras = character(0)))
  }
  wanted <- unique(columns[!tolower(columns) %in% .DROPPED_MANIFEST_COLUMNS])
  unknown <- setdiff(wanted, kept)
  if (length(unknown) > 0L) {
    .cozip_stop("unknown column(s) %s; the manifest has %s",
                .quoted_list(unknown), .quoted_list(kept))
  }
  list(
    head = .REQUIRED_METADATA_COLUMNS,
    extras = setdiff(wanted, .REQUIRED_METADATA_COLUMNS)
  )
}


# Columns are named explicitly. Hive partitioning stays off so the
# manifest's path never adds columns. With a location, the first parameter
# is the GDAL base and the second the manifest path.
.manifest_sql <- function(selected, location) {
  items <- vapply(selected$head, .quote_ident, character(1))
  if (location) {
    items <- c(items, paste(
      "'/vsisubfile/' || \"offset\" || '_' || \"size\" || ',' || ?",
      "AS \"cozip:location\""
    ))
  }
  items <- c(items, vapply(selected$extras, .quote_ident, character(1)))
  sprintf("SELECT %s FROM read_parquet(?, hive_partitioning = false)",
          paste(items, collapse = ", "))
}
