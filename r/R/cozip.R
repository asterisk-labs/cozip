#' Plan a cozip FLAT archive
#'
#' Computes offsets and sizes for the user entries and returns the
#' canonical metadata table plus a row-aligned `paths` data.frame.
#' The function checks each source and reads its file size; it does not read
#' payload contents.
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
  plan <- .plan_flat(table)
  list(metadata = plan$metadata, paths = plan$paths)
}


# Checks the input table, reads each source size once and plans the
# offsets. create() writes with these sizes, so a source that changes
# before the write fails in the C writer instead of leaving a manifest
# that points at the wrong bytes.
.plan_flat <- function(table) {
  .validate_input_table(table)

  n          <- table$num_rows
  user_names <- as.character(table[["name"]])
  user_paths <- as.character(table[["path"]])
  user_sizes <- file.size(user_paths)

  plan_res <- .Call(C_R_cozip_plan, user_names, user_sizes)

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
                          stringsAsFactors = FALSE),
    sizes    = out_sizes
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
  .check_path_arg(out_path, "out_path")
  .check_path_arg(metadata_parquet, "metadata_parquet")
  if (!is.logical(validate) || length(validate) != 1L || is.na(validate)) {
    .cozip_stop("`validate` must be TRUE or FALSE")
  }

  parquet_abs <- normalizePath(metadata_parquet, mustWork = FALSE)

  if (!file.exists(parquet_abs)) {
    .cozip_stop("metadata parquet not found: %s", parquet_abs)
  }
  if (!utils::file_test("-f", parquet_abs)) {
    .cozip_stop("metadata parquet is not a regular file: %s", parquet_abs)
  }
  if (file.size(parquet_abs) == 0) {
    .cozip_stop("metadata parquet is empty")
  }

  .check_parquet_schema(parquet_abs)

  vp         <- .validate_paths_arg(paths)
  user_names <- vp$names
  user_paths <- vp$paths
  n          <- length(user_names)

  plan_res <- .Call(C_R_cozip_plan, user_names, file.size(user_paths))
  planned_sizes <- as.numeric(plan_res$size[seq_len(n)])

  if (validate) {
    .validate_parquet_values(
      parquet_abs,
      user_names,
      as.numeric(plan_res$offset[seq_len(n)]),
      planned_sizes
    )
  }

  .write_flat(out_path, user_names, user_paths, planned_sizes, parquet_abs)
}


# Writes the planned entries plus the manifest and returns the absolute
# path of the archive. R expands `~` itself, so the paths are expanded
# before they reach C. The C writer compares each source with its planned
# size.
.write_flat <- function(out_path, names, paths, sizes, metadata_parquet) {
  out <- path.expand(out_path)
  .Call(C_R_cozip_finalize, out,
        c(names, .META_NAME),
        c(path.expand(paths), metadata_parquet),
        c(sizes, file.size(metadata_parquet)),
        c(rep(FALSE, length(names)), TRUE),
        .PROFILE_FLAT)
  normalizePath(out, mustWork = TRUE)
}


#' Write a cozip archive in one call
#'
#' Plans the archive as [stage_metadata()] does, writes the manifest
#' with [arrow::write_parquet()] and packs the archive with the sizes it
#' planned, so a source that changes in between is an error. The manifest
#' is compressed with zstd, and each column is dictionary encoded only when
#' that writes its first rows smaller. For GeoParquet or custom write
#' options, use the staging functions directly.
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
  .check_path_arg(out_path, "out_path")
  plan <- .plan_flat(table)

  tmp <- .temp_parquet_path(temp_dir)
  on.exit(unlink(tmp, force = TRUE), add = TRUE)
  .write_manifest(plan$metadata, tmp)

  .write_flat(out_path, plan$paths$name, plan$paths$path, plan$sizes, tmp)
}


# Arrow for R cannot set a Parquet encoding per column, so the manifest
# gets zstd and, per column, dictionary encoding only where it writes the
# first rows smaller. Python also tries byte stream split and delta.
.ZSTD_LEVEL <- 9L
.TRIAL_ROWS <- 10000L

.manifest_options <- function() {
  if (arrow::codec_is_available("zstd")) {
    list(compression = "zstd", compression_level = .ZSTD_LEVEL)
  } else {
    list()
  }
}

.parquet_size <- function(table, use_dictionary) {
  sink <- arrow::BufferOutputStream$create()
  do.call(arrow::write_parquet, c(
    list(table, sink, use_dictionary = use_dictionary, write_statistics = TRUE),
    .manifest_options()))
  sink$finish()$size
}

.write_manifest <- function(table, path) {
  sample <- table$Slice(0, min(table$num_rows, .TRIAL_ROWS))
  dictionary <- vapply(seq_len(sample$num_columns) - 1L, function(i) {
    column <- sample$SelectColumns(i)
    .parquet_size(column, TRUE) <= .parquet_size(column, FALSE)
  }, logical(1))
  do.call(arrow::write_parquet, c(
    list(table, path, use_dictionary = dictionary, write_statistics = TRUE),
    .manifest_options()))
  invisible(path)
}


.check_path_arg <- function(value, arg) {
  if (!is.character(value) || length(value) != 1L
      || is.na(value) || !nzchar(value)) {
    .cozip_stop("`%s` must be a single non-empty string", arg)
  }
  invisible(NULL)
}


.validate_input_table <- function(table) {
  if (!inherits(table, "Table")) {
    .cozip_stop("`table` must be an arrow::Table (got %s)",
                paste(class(table), collapse = "/"))
  }

  cols <- names(table)
  .check_column_names(cols, "input table", "reads manifests")

  missing <- setdiff(c("name", "path"), cols)
  if (length(missing) > 0L) {
    .cozip_stop("input table is missing required column(s): %s",
                paste(shQuote(missing), collapse = ", "))
  }

  reserved <- cols[tolower(cols) %in% .RESERVED_INPUT_COLUMNS]
  if (length(reserved) > 0L) {
    .cozip_stop(
      "input table must not contain reserved column(s) %s; the binding computes them (names are compared without case)",
      .quoted_list(reserved)
    )
  }

  if (table$num_rows == 0L) .cozip_stop("empty entry list")

  # Dictionary-encoded strings (R factors) also hold strings.
  for (col in c("name", "path")) {
    type <- table$schema$GetFieldByName(col)$type
    values <- if (inherits(type, "DictionaryType")) type$value_type else type
    if (!.is_string_type(values)) {
      .cozip_stop("`%s` column must contain strings, got %s", col, type$ToString())
    }
  }

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

  bad <- which(!utils::file_test("-f", paths_v))
  if (length(bad) > 0L) {
    i <- bad[1L]
    .cozip_stop("row %d (%s): source is not a regular file: %s",
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

  bad <- which(!utils::file_test("-f", paths_v))
  if (length(bad) > 0L) {
    i <- bad[1L]
    .cozip_stop("`paths` row %d (%s): source is not a regular file: %s",
                i, shQuote(names_v[i]), paths_v[i])
  }

  list(names = names_v, paths = paths_v)
}


.check_parquet_schema <- function(parquet_path) {
  schema <- arrow::ParquetFileReader$create(parquet_path, mmap = FALSE)$GetSchema()
  cols   <- names(schema)
  .check_column_names(cols, "metadata parquet", "reads manifests")

  if ("path" %in% cols) {
    .cozip_stop(
      "metadata parquet must NOT contain a 'path' column; `path` is filesystem-local and does not belong inside the archive"
    )
  }

  protected <- cols[tolower(cols) %in% .PROTECTED_LOCATION_COLUMNS]
  if (length(protected) > 0L) {
    .cozip_stop(
      "metadata parquet must not contain reader-owned column(s): %s",
      .quoted_list(protected)
    )
  }

  missing <- setdiff(.REQUIRED_METADATA_COLUMNS, cols)
  if (length(missing) > 0L) {
    .cozip_stop("metadata parquet is missing required column(s): %s",
                .quoted_list(missing))
  }

  # Spec 13.3: name is a string, offset and size are uint64.
  name_type <- schema$GetFieldByName("name")$type
  if (!.is_string_type(name_type)) {
    .cozip_stop("metadata parquet column 'name' must be a string, got %s",
                name_type$ToString())
  }
  for (col in c("offset", "size")) {
    type <- schema$GetFieldByName(col)$type$ToString()
    if (!identical(type, "uint64")) {
      .cozip_stop(
        "metadata parquet column '%s' must be uint64 (cozip spec 13.3), got %s",
        col, type
      )
    }
  }

  invisible(NULL)
}


.is_string_type <- function(type) {
  type$ToString() %in% c("string", "large_string")
}


.validate_parquet_values <- function(parquet_path, expected_names,
                                     expected_offsets, expected_sizes) {
  tbl <- arrow::read_parquet(
    parquet_path,
    col_select    = c("name", "offset", "size"),
    as_data_frame = FALSE,
    mmap = FALSE
  )

  if (tbl$num_rows != length(expected_names)) {
    .cozip_stop("metadata parquet has %d rows, paths has %d",
                tbl$num_rows, length(expected_names))
  }

  pq_names   <- as.character(tbl[["name"]])
  pq_offsets <- as.numeric(tbl[["offset"]])
  pq_sizes   <- as.numeric(tbl[["size"]])

  if (anyNA(pq_names)) {
    .cozip_stop("metadata parquet has NA name at row %d", which(is.na(pq_names))[1L])
  }
  if (anyNA(pq_offsets)) {
    .cozip_stop("metadata parquet has NA offset at row %d", which(is.na(pq_offsets))[1L])
  }
  if (anyNA(pq_sizes)) {
    .cozip_stop("metadata parquet has NA size at row %d", which(is.na(pq_sizes))[1L])
  }

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


#' @rdname create
#' @export
write <- create
