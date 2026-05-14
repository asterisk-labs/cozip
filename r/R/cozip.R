#' Plan a cozip FLAT archive
#'
#' Computes offsets and sizes for the user entries and returns the
#' canonical metadata table plus a row-aligned `paths` data.frame.
#' No I/O.
#'
#' If you reorder `metadata` before writing it, reorder `paths` the
#' same way or [stage_create()] will reject the mismatch.
#'
#' @param table arrow::Table with `name` and `path`. Extras are
#'   preserved in the metadata.
#'
#' @return List with `metadata` (arrow::Table) and `paths`
#'   (data.frame).
#'
#' @export
stage_metadata <- function(table) {
  .validate_input_table(table)

  n          <- table$num_rows
  user_names <- as.character(table[["name"]])
  user_paths <- as.character(table[["path"]])
  user_sizes <- file.size(user_paths)

  all_names  <- c(user_names,    .META_NAME)
  all_sizes  <- c(user_sizes,    0)            # placeholder
  all_in_idx <- c(rep(FALSE, n), TRUE)

  plan_res <- .Call(C_R_cozip_plan, all_names, all_sizes, all_in_idx)

  out_offsets <- as.numeric(plan_res$offset[seq_len(n)])
  out_sizes   <- as.numeric(plan_res$size[seq_len(n)])

  extras <- setdiff(names(table), c("name", "path"))
  build_args <- list(
    name   = arrow::Array$create(user_names),
    offset = arrow::Array$create(out_offsets, type = arrow::uint64()),
    size   = arrow::Array$create(out_sizes,   type = arrow::uint64())
  )
  for (col in extras) build_args[[col]] <- table[[col]]

  meta <- do.call(arrow::arrow_table, build_args)

  md <- table$schema$metadata
  if (!is.null(md)) meta <- meta$ReplaceSchemaMetadata(md)

  list(
    metadata = meta,
    paths    = data.frame(name = user_names, path = user_paths,
                          stringsAsFactors = FALSE)
  )
}


#' Pack a cozip archive from sources and a user-written parquet
#'
#' The metadata parquet is embedded verbatim, so any GeoParquet or
#' schema metadata you wrote is preserved.
#'
#' @param out_path Destination archive path.
#' @param paths data.frame with `name` and `path` in the same row
#'   order as `metadata_parquet`.
#' @param metadata_parquet Path to a parquet with `name`, `offset`,
#'   `size` columns and no `path` column.
#' @param validate If `TRUE`, re-runs the plan and checks that the
#'   parquet matches.
#'
#' @return Absolute path of the created archive.
#'
#' @export
stage_create <- function(out_path, paths, metadata_parquet,
                         validate = TRUE) {
  if (!is.character(out_path) || length(out_path) != 1L
      || is.na(out_path)) {
    .cozip_stop("`out_path` must be a single non-NA string")
  }

  out_abs     <- normalizePath(out_path,         mustWork = FALSE)
  parquet_abs <- normalizePath(metadata_parquet, mustWork = FALSE)

  if (!file.exists(parquet_abs)) {
    .cozip_stop("metadata parquet not found: %s", parquet_abs)
  }

  .check_parquet_schema(parquet_abs)

  vp         <- .validate_paths_arg(paths)
  user_names <- vp$names
  user_paths <- vp$paths
  user_sizes <- file.size(user_paths)
  n          <- length(user_names)

  all_names  <- c(user_names,    .META_NAME)
  all_sizes  <- c(user_sizes,    0)
  all_in_idx <- c(rep(FALSE, n), TRUE)

  plan_res <- .Call(C_R_cozip_plan, all_names, all_sizes, all_in_idx)

  if (isTRUE(validate)) {
    .validate_parquet_values(
      parquet_abs,
      user_names,
      as.numeric(plan_res$offset[seq_len(n)]),
      as.numeric(plan_res$size[seq_len(n)])
    )
  }

  fin_names  <- c(user_names,    .META_NAME)
  fin_paths  <- c(user_paths,    parquet_abs)
  fin_sizes  <- c(user_sizes,    file.size(parquet_abs))
  fin_in_idx <- c(rep(FALSE, n), TRUE)

  .Call(C_R_cozip_finalize, out_abs, fin_names, fin_paths,
        fin_sizes, fin_in_idx, .PROFILE_FLAT)

  out_abs
}


#' Write a cozip archive in one call
#'
#' Wraps [stage_metadata()] + [arrow::write_parquet()] +
#' [stage_create()]. For GeoParquet or custom write options, use
#' the staging functions directly.
#'
#' @param out_path Destination archive path.
#' @param table arrow::Table with `name` and `path`.
#' @param temp_dir Directory for the temp metadata parquet
#'   (defaults to [tempdir()]).
#'
#' @return Absolute path of the created archive.
#'
#' @export
create <- function(out_path, table, temp_dir = NULL) {
  res <- stage_metadata(table)

  tmp <- .temp_parquet_path(temp_dir)
  on.exit(unlink(tmp, force = TRUE), add = TRUE)
  arrow::write_parquet(res$metadata, tmp)

  stage_create(out_path, res$paths, tmp, validate = FALSE)
}


.validate_input_table <- function(table) {
  if (!inherits(table, "Table")) {
    .cozip_stop("`table` must be an arrow::Table (got %s)",
                paste(class(table), collapse = "/"))
  }

  cols    <- names(table)
  missing <- setdiff(c("name", "path"), cols)
  if (length(missing) > 0L) {
    .cozip_stop("input table is missing required column(s): %s",
                paste(shQuote(missing), collapse = ", "))
  }

  reserved <- intersect(.RESERVED_INPUT_COLUMNS, cols)
  if (length(reserved) > 0L) {
    .cozip_stop(
      "input table must not contain reserved column(s) %s; the binding computes them",
      paste(shQuote(reserved), collapse = ", ")
    )
  }

  if (table$num_rows == 0L) .cozip_stop("empty entry list")

  names_v <- as.character(table[["name"]])
  paths_v <- as.character(table[["path"]])

  if (anyNA(names_v)) .cozip_stop("`name` column has NA values")
  if (anyNA(paths_v)) .cozip_stop("`path` column has NA values")

  hits <- which(names_v %in% .RESERVED_NAMES)
  if (length(hits) > 0L) {
    .cozip_stop("row %d uses reserved name %s",
                hits[1L], shQuote(names_v[hits[1L]]))
  }

  dups <- which(duplicated(names_v))
  if (length(dups) > 0L) {
    .cozip_stop("duplicate name %s at row %d",
                shQuote(names_v[dups[1L]]), dups[1L])
  }

  bad <- which(!file.exists(paths_v))
  if (length(bad) > 0L) {
    i <- bad[1L]
    .cozip_stop("row %d (%s): source file not found: %s",
                i, shQuote(names_v[i]), paths_v[i])
  }

  invisible(NULL)
}


.validate_paths_arg <- function(paths) {
  if (!is.data.frame(paths)) {
    .cozip_stop("`paths` must be a data.frame with `name` and `path` columns")
  }

  cols    <- colnames(paths)
  missing <- setdiff(c("name", "path"), cols)
  if (length(missing) > 0L) {
    .cozip_stop("`paths` is missing required column(s): %s",
                paste(shQuote(missing), collapse = ", "))
  }

  if (nrow(paths) == 0L) .cozip_stop("empty `paths` data.frame")

  names_v <- paths$name
  paths_v <- paths$path

  if (!is.character(names_v) || anyNA(names_v)) {
    .cozip_stop("`paths$name` must be character with no NAs")
  }
  if (!is.character(paths_v) || anyNA(paths_v)) {
    .cozip_stop("`paths$path` must be character with no NAs")
  }

  hits <- which(names_v %in% .RESERVED_NAMES)
  if (length(hits) > 0L) {
    .cozip_stop("`paths` row %d uses reserved name %s",
                hits[1L], shQuote(names_v[hits[1L]]))
  }

  dups <- which(duplicated(names_v))
  if (length(dups) > 0L) {
    .cozip_stop("duplicate name %s at `paths` row %d",
                shQuote(names_v[dups[1L]]), dups[1L])
  }

  bad <- which(!file.exists(paths_v))
  if (length(bad) > 0L) {
    i <- bad[1L]
    .cozip_stop("`paths` row %d (%s): source not found: %s",
                i, shQuote(names_v[i]), paths_v[i])
  }

  list(names = names_v, paths = paths_v)
}


.check_parquet_schema <- function(parquet_path) {
  schema <- arrow::ParquetFileReader$create(parquet_path)$GetSchema()
  cols   <- names(schema)

  if ("path" %in% cols) {
    .cozip_stop(
      "metadata parquet must NOT contain a 'path' column; `path` is filesystem-local and does not belong inside the archive"
    )
  }

  missing <- setdiff(.REQUIRED_METADATA_COLUMNS, cols)
  if (length(missing) > 0L) {
    .cozip_stop("metadata parquet is missing required column(s): %s",
                paste(shQuote(missing), collapse = ", "))
  }

  invisible(NULL)
}


.validate_parquet_values <- function(parquet_path, expected_names,
                                     expected_offsets, expected_sizes) {
  tbl <- arrow::read_parquet(
    parquet_path,
    col_select    = c("name", "offset", "size"),
    as_data_frame = FALSE
  )

  if (tbl$num_rows != length(expected_names)) {
    .cozip_stop("metadata parquet has %d rows, paths has %d",
                tbl$num_rows, length(expected_names))
  }

  pq_names   <- as.character(tbl[["name"]])
  pq_offsets <- as.numeric(tbl[["offset"]])
  pq_sizes   <- as.numeric(tbl[["size"]])

  if (!identical(pq_names, expected_names)) {
    i <- which(pq_names != expected_names)[1]
    .cozip_stop("name mismatch at row %d: parquet=%s, paths=%s",
                i, shQuote(pq_names[i]), shQuote(expected_names[i]))
  }
  if (!identical(pq_offsets, expected_offsets)) {
    i <- which(pq_offsets != expected_offsets)[1]
    .cozip_stop("offset mismatch at row %d (%s): parquet=%g, plan=%g",
                i, shQuote(expected_names[i]),
                pq_offsets[i], expected_offsets[i])
  }
  if (!identical(pq_sizes, expected_sizes)) {
    i <- which(pq_sizes != expected_sizes)[1]
    .cozip_stop("size mismatch at row %d (%s): parquet=%g, plan=%g",
                i, shQuote(expected_names[i]),
                pq_sizes[i], expected_sizes[i])
  }

  invisible(NULL)
}


.temp_parquet_path <- function(temp_dir) {
  if (is.null(temp_dir)) temp_dir <- tempdir()
  if (!dir.exists(temp_dir)) dir.create(temp_dir, recursive = TRUE)
  tempfile(tmpdir = temp_dir, fileext = ".parquet")
}