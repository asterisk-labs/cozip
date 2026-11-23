# Mirror of @pytest.fixture archive.
local_archive <- function(envir = parent.frame()) {
  tmp <- withr::local_tempdir(.local_envir = envir)
  fix <- make_fixtures(tmp)
  tbl <- make_input_table(fix)
  arc <- make_archive(tmp, tbl)
  list(
    tmp_path      = tmp,
    fixtures      = fix,
    input_table   = tbl,
    archive       = arc,
    archive_bytes = readBin(arc, raw(), n = file.size(arc))
  )
}

# R's utils::unzip doesn't expose GP flags or compression method,
# so walk the Central Directory by hand.
parse_cd <- function(bytes) {
  n <- length(bytes)

  # EOCD: last 22 bytes, no archive comment.
  eocd <- n - 21L
  cd_size   <- read_u32_le(bytes[(eocd + 12L):(eocd + 15L)])
  cd_offset <- read_u32_le(bytes[(eocd + 16L):(eocd + 19L)])

  cur <- cd_offset + 1L
  end <- cd_offset + cd_size
  entries <- list()
  CD_SIG <- as.raw(c(0x50, 0x4B, 0x01, 0x02))

  while (cur <= end) {
    stopifnot(identical(bytes[cur:(cur + 3L)], CD_SIG))
    flag    <- read_u16_le(bytes[(cur +  8L):(cur +  9L)])
    method  <- read_u16_le(bytes[(cur + 10L):(cur + 11L)])
    fn_len  <- read_u16_le(bytes[(cur + 28L):(cur + 29L)])
    ex_len  <- read_u16_le(bytes[(cur + 30L):(cur + 31L)])
    cm_len  <- read_u16_le(bytes[(cur + 32L):(cur + 33L)])
    fn      <- rawToChar(bytes[(cur + 46L):(cur + 46L + fn_len - 1L)])
    entries[[length(entries) + 1L]] <- list(
      filename = fn, flag_bits = flag, compress_method = method
    )
    cur <- cur + 46L + fn_len + ex_len + cm_len
  }
  entries
}


describe("Spec invariants: bytes of create() output", {

  it("archive size meets minimum", {
    ctx <- local_archive()
    expect_gte(file.size(ctx$archive), HASH_WINDOW + INDEX_OFFSET)
  })

  it("LFH signature", {
    ctx <- local_archive()
    expect_identical(ctx$archive_bytes[1:4], ZIP_LFH_SIG)
  })

  it("index entry filename", {
    ctx <- local_archive()
    expect_identical(ctx$archive_bytes[31:39], INDEX_NAME)
  })

  it("hash block header", {
    ctx <- local_archive()
    expect_identical(ctx$archive_bytes[40:43], HASH_BLOCK_HEADER)
  })

  it("index header (magic + version + profile)", {
    ctx <- local_archive()
    b <- ctx$archive_bytes
    expect_identical(b[52:55], INDEX_MAGIC)
    expect_identical(b[56:57], INDEX_VERSION)
    expect_equal(as.integer(b[58]), PROFILE_FLAT)
  })

  it("EOCD comment is empty", {
    ctx <- local_archive()
    n <- length(ctx$archive_bytes)
    expect_identical(ctx$archive_bytes[(n - 1L):n], as.raw(c(0x00, 0x00)))
  })

  it("integrity hash matches FNV-1a 64", {
    ctx <- local_archive()
    b  <- ctx$archive_bytes
    sz <- index_size_from_lfh(b)
    expected <- fnv1a_64(hash_input(b, sz))
    sb <- b[44:51]
    stored <- c(hi = read_u32_le(sb[5:8]), lo = read_u32_le(sb[1:4]))
    expect_identical(stored, expected)
  })

  it("index lists exactly __metadata__", {
    ctx <- local_archive()
    b  <- ctx$archive_bytes
    sz <- index_size_from_lfh(b)
    entries <- parse_index(b[(INDEX_OFFSET + 1L):(INDEX_OFFSET + sz)])
    expect_setequal(names(entries), "__metadata__")

    meta <- entries[["__metadata__"]]
    expect_gt(meta[["offset"]], 0)
    expect_lt(meta[["offset"]], length(b))
    expect_gt(meta[["size"]],   0)
    expect_lte(meta[["size"]],  length(b) - meta[["offset"]])
  })

  it("manifest is compressed with zstd", {
    skip_if_not(arrow::codec_is_available("zstd"))
    ctx <- local_archive()
    b  <- ctx$archive_bytes
    sz <- index_size_from_lfh(b)
    meta <- parse_index(b[(INDEX_OFFSET + 1L):(INDEX_OFFSET + sz)])[["__metadata__"]]
    pq <- withr::local_tempfile(fileext = ".parquet")
    writeBin(b[(meta[["offset"]] + 1):(meta[["offset"]] + meta[["size"]])], pq)
    con <- DBI::dbConnect(duckdb::duckdb())
    on.exit(DBI::dbDisconnect(con, shutdown = TRUE), add = TRUE)
    codecs <- DBI::dbGetQuery(con,
      "SELECT DISTINCT compression FROM parquet_metadata(?)", params = list(pq))
    expect_identical(codecs$compression, "ZSTD")
  })
})


describe("ZIP compatibility: readable by standard ZIP tools", {

  it("stdlib reader lists all entries", {
    ctx <- local_archive()
    nm <- utils::unzip(ctx$archive, list = TRUE)$Name
    expect_true(all(c("__cozip__", "__metadata__", "a.txt", "b.bin") %in% nm))
  })

  it("all entries use STORE", {
    ctx <- local_archive()
    cd <- parse_cd(ctx$archive_bytes)
    for (e in cd) {
      expect_equal(e$compress_method, 0L,
                   info = sprintf("entry '%s' is not STORE", e$filename))
    }
  })

  it("no encryption, no data descriptor", {
    ctx <- local_archive()
    cd <- parse_cd(ctx$archive_bytes)
    for (e in cd) {
      expect_equal(bitwAnd(e$flag_bits, 0x01), 0L,
                   info = sprintf("'%s' encrypted", e$filename))
      expect_equal(bitwAnd(e$flag_bits, 0x08), 0L,
                   info = sprintf("'%s' has DD", e$filename))
    }
  })

  it("user payloads byte-exact", {
    ctx <- local_archive()
    out_dir <- file.path(ctx$tmp_path, "extract")
    dir.create(out_dir)
    utils::unzip(ctx$archive, files = c("a.txt", "b.bin"), exdir = out_dir)
    rec_a <- readBin(file.path(out_dir, "a.txt"), raw(), n = length(SMALL_CONTENT))
    rec_b <- readBin(file.path(out_dir, "b.bin"), raw(), n = length(MEDIUM_CONTENT))
    expect_identical(rec_a, SMALL_CONTENT)
    expect_identical(rec_b, MEDIUM_CONTENT)
  })
})


describe("stage_metadata: layout planner", {

  it("drops path column", {
    ctx <- local_archive()
    meta <- cozip::stage_metadata(ctx$input_table)$metadata
    expect_false("path" %in% names(meta))
  })

  it("adds offset and size as uint64", {
    ctx <- local_archive()
    meta <- cozip::stage_metadata(ctx$input_table)$metadata
    expect_identical(meta$schema$GetFieldByName("offset")$type$ToString(), "uint64")
    expect_identical(meta$schema$GetFieldByName("size")$type$ToString(),   "uint64")
  })

  it("preserves user extras", {
    ctx <- local_archive()
    meta <- cozip::stage_metadata(ctx$input_table)$metadata
    expect_identical(as.character(meta[["category"]]), c("text", "binary"))
  })

  it("canonical column order", {
    ctx <- local_archive()
    meta <- cozip::stage_metadata(ctx$input_table)$metadata
    expect_identical(names(meta)[1:3], c("name", "offset", "size"))
  })

  it("sizes match source lengths", {
    ctx <- local_archive()
    meta <- cozip::stage_metadata(ctx$input_table)$metadata
    sizes <- as.numeric(meta[["size"]])
    expect_equal(sizes, c(length(SMALL_CONTENT), length(MEDIUM_CONTENT)))
  })

  it("offsets strictly increasing", {
    ctx <- local_archive()
    meta <- cozip::stage_metadata(ctx$input_table)$metadata
    offsets <- as.numeric(meta[["offset"]])
    expect_true(all(diff(offsets) > 0))
  })

  it("returns paths aligned with metadata", {
    ctx <- local_archive()
    res <- cozip::stage_metadata(ctx$input_table)
    expect_equal(res$paths$name, c("a.txt", "b.bin"))
    expect_equal(res$paths$path, c(ctx$fixtures$small, ctx$fixtures$medium))
    expect_equal(nrow(res$paths), res$metadata$num_rows)
  })

  it("paths drive stage_create end-to-end", {
    ctx <- local_archive()
    res <- cozip::stage_metadata(ctx$input_table)
    pq  <- file.path(ctx$tmp_path, "meta.parquet")
    arrow::write_parquet(res$metadata, pq)
    out <- file.path(ctx$tmp_path, "out.zip")
    cozip::stage_create(out, res$paths, pq)
    assert_valid_cozip(readBin(out, raw(), n = file.size(out)))
  })

  it("rejects offset in input", {
    ctx <- local_archive()
    bad <- arrow::arrow_table(
      name   = "a.txt",
      path   = ctx$fixtures$small,
      offset = 0
    )
    expect_error(cozip::stage_metadata(bad), "reserved")
  })

  it("rejects reader-owned columns in input", {
    ctx <- local_archive()
    for (column in c("cozip:location", "taco:location")) {
      values <- list(
        name = "a.txt",
        path = ctx$fixtures$small
      )
      values[[column]] <- "must-not-be-stored"
      bad <- do.call(arrow::arrow_table, values)
      expect_error(cozip::stage_metadata(bad), "reserved")
    }
  })

  it("rejects duplicate names", {
    ctx <- local_archive()
    bad <- arrow::arrow_table(
      name = c("x", "x"),
      path = c(ctx$fixtures$small, ctx$fixtures$medium)
    )
    expect_error(cozip::stage_metadata(bad), "duplicate")
  })

  it("rejects reserved archive name", {
    ctx <- local_archive()
    bad <- arrow::arrow_table(
      name = "__metadata__",
      path = ctx$fixtures$small
    )
    expect_error(cozip::stage_metadata(bad), "reserved")
  })

  it("rejects reserved columns in any case", {
    ctx <- local_archive()
    for (column in c("Offset", "SIZE", "COZIP:LOCATION", "Taco:Location")) {
      values <- list(name = "a.txt", path = ctx$fixtures$small)
      values[[column]] <- "x"
      bad <- do.call(arrow::arrow_table, values)
      expect_error(
        cozip::stage_metadata(bad),
        sprintf("reserved column(s) '%s'; the binding computes them (names are compared without case)", column),
        fixed = TRUE
      )
    }
  })

  it("rejects input columns that differ only in case", {
    ctx <- local_archive()
    bad <- arrow::arrow_table(
      name = "a.txt", path = ctx$fixtures$small, Tag = "x", tag = "y"
    )
    expect_error(
      cozip::stage_metadata(bad),
      "columns that differ only in case: 'Tag' and 'tag'; DuckDB, which reads manifests, cannot tell them apart",
      fixed = TRUE
    )
    bad <- arrow::arrow_table(name = "a.txt", path = ctx$fixtures$small, NAME = "x")
    expect_error(cozip::stage_metadata(bad), "'name' and 'NAME'", fixed = TRUE)
  })

  it("rejects names and paths that are not strings", {
    ctx <- local_archive()
    expect_error(
      cozip::stage_metadata(arrow::arrow_table(name = 1L, path = ctx$fixtures$small)),
      "`name` column must contain strings, got int32",
      fixed = TRUE
    )
    expect_error(
      cozip::create(file.path(ctx$tmp_path, "n.zip"),
                    arrow::arrow_table(name = 1L, path = ctx$fixtures$small)),
      "`name` column must contain strings",
      fixed = TRUE
    )
    expect_error(
      cozip::stage_metadata(arrow::arrow_table(name = "a.txt", path = 1)),
      "`path` column must contain strings, got double",
      fixed = TRUE
    )
  })

  it("accepts dictionary-encoded names", {
    ctx <- local_archive()
    table <- arrow::arrow_table(
      name = factor(c("a.txt", "b.bin")),
      path = c(ctx$fixtures$small, ctx$fixtures$medium)
    )
    res <- cozip::stage_metadata(table)
    expect_identical(res$paths$name, c("a.txt", "b.bin"))
  })

  it("rejects zero-byte sources", {
    tmp <- withr::local_tempdir()
    empty <- file.path(tmp, "empty.bin")
    file.create(empty)
    bad <- arrow::arrow_table(name = "empty.bin", path = empty)
    expect_error(cozip::stage_metadata(bad), "zero-byte payload")
  })

  it("rejects directory sources", {
    tmp <- withr::local_tempdir()
    bad <- arrow::arrow_table(name = "directory", path = tmp)
    expect_error(cozip::stage_metadata(bad), "not a regular file")
  })

  it("rejects non-ASCII archive names", {
    ctx <- local_archive()
    bad <- arrow::arrow_table(name = "niño.bin", path = ctx$fixtures$small)
    expect_error(cozip::stage_metadata(bad), "non-ASCII")
  })

  it("accepts Unicode writer paths", {
    tmp <- withr::local_tempdir()
    source <- file.path(tmp, "niño.bin")
    writeBin(charToRaw("x"), source)
    output <- file.path(tmp, "salida-niño.zip")
    table <- arrow::arrow_table(name = "data.bin", path = source)

    cozip::create(output, table)

    expect_true(file.exists(output))
    assert_valid_cozip(readBin(output, raw(), n = file.size(output)))
  })
})


describe("read input validation", {

  it("rejects an empty path before reading", {
    expect_error(cozip::read(""), "non-empty")
  })

  it("rejects non-ASCII paths before reading", {
    expect_error(cozip::read("niño.zip"), "ASCII paths and URLs only")
  })

  it("rejects empty selected column names before reading", {
    expect_error(cozip::read("archive.zip", columns = ""), "non-empty")
  })
})


describe("stage_create: embed user-written parquet verbatim", {

  write_meta <- function(ctx) {
    pq <- file.path(ctx$tmp_path, "meta.parquet")
    arrow::write_parquet(cozip::stage_metadata(ctx$input_table)$metadata, pq)
    pq
  }

  it("requires validate to be one TRUE or FALSE value", {
    expect_error(
      cozip::stage_create("out.zip", data.frame(), "metadata.parquet",
                          validate = "yes"),
      "`validate` must be TRUE or FALSE",
      fixed = TRUE
    )
  })

  it("packs a valid archive", {
    ctx <- local_archive()
    pq  <- write_meta(ctx)
    out <- file.path(ctx$tmp_path, "out.zip")
    cozip::stage_create(out, make_paths_arg(ctx$input_table), pq)
    assert_valid_cozip(readBin(out, raw(), n = file.size(out)))
  })

  it("metadata parquet embedded verbatim", {
    ctx <- local_archive()
    pq  <- write_meta(ctx)
    original <- readBin(pq, raw(), n = file.size(pq))

    out <- file.path(ctx$tmp_path, "out.zip")
    cozip::stage_create(out, make_paths_arg(ctx$input_table), pq)

    arc      <- readBin(out, raw(), n = file.size(out))
    embedded <- extract_metadata_payload(arc)
    expect_identical(embedded, original)
  })

  it("rejects parquet with path column", {
    ctx <- local_archive()
    md_df <- as.data.frame(cozip::stage_metadata(ctx$input_table)$metadata)
    md_df$path <- c("/tmp/a", "/tmp/b")
    pq <- file.path(ctx$tmp_path, "meta.parquet")
    arrow::write_parquet(arrow::arrow_table(md_df), pq)

    expect_error(
      cozip::stage_create(file.path(ctx$tmp_path, "out.zip"),
                          make_paths_arg(ctx$input_table), pq),
      "path"
    )
  })

  it("rejects reader-owned columns in parquet in any case", {
    ctx <- local_archive()
    for (column in c("cozip:location", "taco:location", "COZIP:LOCATION",
                     "Taco:Location")) {
      md_df <- as.data.frame(cozip::stage_metadata(ctx$input_table)$metadata)
      md_df[[column]] <- "must-not-be-stored"
      pq <- file.path(ctx$tmp_path, paste0(gsub(":", "-", column), ".parquet"))
      arrow::write_parquet(arrow::arrow_table(md_df), pq)

      expect_error(
        cozip::stage_create(file.path(ctx$tmp_path, "out.zip"),
                            make_paths_arg(ctx$input_table), pq),
        "reader-owned"
      )
    }
  })

  it("rejects parquet columns that differ only in case", {
    ctx <- local_archive()
    md <- cozip::stage_metadata(ctx$input_table)$metadata
    md$Offset <- arrow::Array$create(c(1, 2))
    pq <- file.path(ctx$tmp_path, "case.parquet")
    arrow::write_parquet(md, pq)

    expect_error(
      cozip::stage_create(file.path(ctx$tmp_path, "out.zip"),
                          make_paths_arg(ctx$input_table), pq),
      "metadata parquet has columns that differ only in case: 'offset' and 'Offset'",
      fixed = TRUE
    )
  })

  it("requires a string name and uint64 offset and size", {
    ctx <- local_archive()
    md <- cozip::stage_metadata(ctx$input_table)$metadata
    u64 <- function(x) arrow::Array$create(x, type = arrow::uint64())
    cases <- list(
      list(
        table = arrow::arrow_table(name = 1:2, offset = md$offset, size = md$size),
        message = "column 'name' must be a string, got int32"
      ),
      list(
        table = arrow::arrow_table(
          name = md$name, offset = md$offset$cast(arrow::int64()), size = md$size
        ),
        message = "column 'offset' must be uint64 (cozip spec 13.3), got int64"
      ),
      list(
        table = arrow::arrow_table(
          name = md$name, offset = md$offset, size = as.numeric(md$size)
        ),
        message = "column 'size' must be uint64 (cozip spec 13.3), got double"
      )
    )
    for (case in cases) {
      pq <- file.path(ctx$tmp_path, "typed.parquet")
      arrow::write_parquet(case$table, pq)
      expect_error(
        cozip::stage_create(file.path(ctx$tmp_path, "out.zip"),
                            make_paths_arg(ctx$input_table), pq,
                            validate = FALSE),
        case$message,
        fixed = TRUE
      )
    }
  })

  it("returns the absolute path of a new archive", {
    ctx <- local_archive()
    pq <- write_meta(ctx)
    withr::local_dir(ctx$tmp_path)
    out <- cozip::stage_create("relative.zip", make_paths_arg(ctx$input_table), pq)
    expect_identical(out, normalizePath(file.path(ctx$tmp_path, "relative.zip")))
    expect_true(file.exists(out))
  })

  it("rejects parquet missing required columns", {
    ctx <- local_archive()
    bad <- arrow::arrow_table(name = c("a.txt", "b.bin"))
    pq  <- file.path(ctx$tmp_path, "meta.parquet")
    arrow::write_parquet(bad, pq)

    expect_error(
      cozip::stage_create(file.path(ctx$tmp_path, "out.zip"),
                          make_paths_arg(ctx$input_table), pq),
      "required column"
    )
  })

  it("rejects an empty metadata file", {
    ctx <- local_archive()
    pq <- file.path(ctx$tmp_path, "empty.parquet")
    file.create(pq)

    expect_error(
      cozip::stage_create(file.path(ctx$tmp_path, "out.zip"),
                          make_paths_arg(ctx$input_table), pq),
      "metadata parquet is empty"
    )
  })

  it("rejects null metadata values", {
    ctx <- local_archive()
    md <- cozip::stage_metadata(ctx$input_table)$metadata
    md$name <- arrow::Array$create(c("a.txt", NA_character_))
    pq <- file.path(ctx$tmp_path, "null.parquet")
    arrow::write_parquet(md, pq)

    expect_error(
      cozip::stage_create(file.path(ctx$tmp_path, "out.zip"),
                          make_paths_arg(ctx$input_table), pq),
      "NA name at row 2"
    )
  })

  it("rejects offset mismatch", {
    ctx <- local_archive()
    bad <- arrow::arrow_table(
      name   = c("a.txt", "b.bin"),
      offset = arrow::Array$create(c(0, 0), type = arrow::uint64()),
      size   = arrow::Array$create(
        c(length(SMALL_CONTENT), length(MEDIUM_CONTENT)),
        type = arrow::uint64()
      )
    )
    pq <- file.path(ctx$tmp_path, "meta.parquet")
    arrow::write_parquet(bad, pq)

    expect_error(
      cozip::stage_create(file.path(ctx$tmp_path, "out.zip"),
                          make_paths_arg(ctx$input_table), pq),
      "offset mismatch"
    )
  })

  it("rejects row count mismatch", {
    ctx <- local_archive()
    bad <- arrow::arrow_table(
      name   = "a.txt",
      offset = arrow::Array$create(0, type = arrow::uint64()),
      size   = arrow::Array$create(0, type = arrow::uint64())
    )
    pq <- file.path(ctx$tmp_path, "meta.parquet")
    arrow::write_parquet(bad, pq)

    expect_error(
      cozip::stage_create(file.path(ctx$tmp_path, "out.zip"),
                          make_paths_arg(ctx$input_table), pq),
      "rows"
    )
  })
})


describe("schema metadata round-trips bit-perfect", {

  it("Arrow schema KV is preserved end-to-end", {
    ctx <- local_archive()

    geo_value <- paste0(
      '{"version":"1.0.0","primary_column":"geometry",',
      '"columns":{"geometry":{"encoding":"WKB","crs":null}}}'
    )

    tagged <- cozip::stage_metadata(ctx$input_table)$metadata$ReplaceSchemaMetadata(
      list(geo = geo_value, `asterisk:tag` = "cozip-conformance")
    )

    pq <- file.path(ctx$tmp_path, "meta.parquet")
    arrow::write_parquet(tagged, pq)

    out <- file.path(ctx$tmp_path, "out.zip")
    cozip::stage_create(out, make_paths_arg(ctx$input_table), pq)

    arc      <- readBin(out, raw(), n = file.size(out))
    embedded <- extract_metadata_payload(arc)

    rec_pq <- file.path(ctx$tmp_path, "recovered.parquet")
    writeBin(embedded, rec_pq)
    recovered <- arrow::read_parquet(rec_pq, as_data_frame = FALSE)

    md <- recovered$schema$metadata
    expect_identical(md[["geo"]],           geo_value)
    expect_identical(md[["asterisk:tag"]], "cozip-conformance")
  })
})


describe("GeoParquet round-trip via sf + geoarrow", {

  it("sf reads embedded GeoParquet with CRS, names, and coords intact", {
    skip_if_not_installed("sf")
    skip_if_not_installed("geoarrow")

    suppressPackageStartupMessages(library(geoarrow))
    on.exit(try(detach("package:geoarrow", unload = TRUE), silent = TRUE),
            add = TRUE)

    ctx <- local_archive()

    md_df <- as.data.frame(cozip::stage_metadata(ctx$input_table)$metadata)
    geom <- sf::st_sfc(
      sf::st_point(c(-77.04, -12.05)),
      sf::st_point(c( 2.35,  48.86)),
      crs = 4326
    )
    md_sf <- sf::st_sf(md_df, geometry = geom)

    # as.data.frame() turns uint64 into R numbers; spec 13.3 needs uint64.
    md <- arrow::as_arrow_table(md_sf)
    md$offset <- md$offset$cast(arrow::uint64())
    md$size <- md$size$cast(arrow::uint64())
    pq <- file.path(ctx$tmp_path, "meta.parquet")
    arrow::write_parquet(md, pq)

    out <- file.path(ctx$tmp_path, "out.zip")
    cozip::stage_create(out, make_paths_arg(ctx$input_table), pq)

    arc      <- readBin(out, raw(), n = file.size(out))
    embedded <- extract_metadata_payload(arc)

    rec_pq <- file.path(ctx$tmp_path, "recovered.parquet")
    writeBin(embedded, rec_pq)
    recovered <- arrow::read_parquet(rec_pq)
    recovered$geometry <- sf::st_as_sfc(recovered$geometry)

    expect_s3_class(recovered, "sf")
    expect_equal(sf::st_crs(recovered)$epsg, 4326L)
    expect_identical(recovered$name, c("a.txt", "b.bin"))

    coords <- sf::st_coordinates(recovered)
    expect_equal(unname(coords[1, "X"]), -77.04)
    expect_equal(unname(coords[1, "Y"]), -12.05)
    expect_equal(unname(coords[2, "X"]),   2.35)
    expect_equal(unname(coords[2, "Y"]),  48.86)
  })
})


describe("create", {

  it("returns the absolute path of a new archive", {
    tmp <- withr::local_tempdir()
    table <- make_input_table(make_fixtures(tmp))
    withr::local_dir(tmp)
    out <- cozip::create("relative.zip", table)
    expect_identical(out, normalizePath(file.path(tmp, "relative.zip")))
    expect_true(file.exists(out))
    expect_identical(cozip::write("again.zip", table),
                     normalizePath(file.path(tmp, "again.zip")))
  })

  it("expands ~ in source, metadata and output paths", {
    skip_on_os("windows")
    tmp <- withr::local_tempdir()
    fixtures <- make_fixtures(tmp)
    home <- normalizePath(path.expand("~"), mustWork = FALSE)
    skip_if_not(startsWith(home, "/") && dir.exists(home))
    # A ~ path that leads from the home directory back to the root.
    up <- paste0("~", strrep("/..", length(strsplit(home, "/", fixed = TRUE)[[1]]) - 1L))
    tilde <- function(path) paste0(up, normalizePath(path, mustWork = FALSE))
    table <- arrow::arrow_table(
      name = c("a.txt", "b.bin"),
      path = c(tilde(fixtures$small), tilde(fixtures$medium))
    )

    out <- cozip::create(tilde(file.path(tmp, "tilde.zip")), table)
    expect_identical(out, normalizePath(file.path(tmp, "tilde.zip")))
    expect_identical(cozip::read(out)$name, c("a.txt", "b.bin"))

    res <- cozip::stage_metadata(table)
    pq <- file.path(tmp, "meta.parquet")
    arrow::write_parquet(res$metadata, pq)
    staged <- cozip::stage_create(tilde(file.path(tmp, "staged.zip")), res$paths,
                                  tilde(pq))
    expect_identical(staged, normalizePath(file.path(tmp, "staged.zip")))
    assert_valid_cozip(readBin(staged, raw(), n = file.size(staged)))
  })

  it("fails when a source changes size after planning", {
    tmp <- withr::local_tempdir()
    fixtures <- make_fixtures(tmp)
    plan_flat <- cozip:::.plan_flat
    local_mocked_bindings(
      .plan_flat = function(table) {
        plan <- plan_flat(table)
        cat("grown", file = fixtures$small, append = TRUE)
        plan
      },
      .package = "cozip"
    )
    out <- file.path(tmp, "out.zip")
    expect_error(
      cozip::create(out, make_input_table(fixtures)),
      sprintf("source is %d bytes, but the planned payload size is %d",
              length(SMALL_CONTENT) + 5L, length(SMALL_CONTENT)),
      fixed = TRUE
    )
    expect_false(file.exists(out))
  })
})
