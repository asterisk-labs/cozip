# Real reads of Flat archives through the native reader.

stored_manifest <- function(archive) {
  bytes <- readBin(archive, raw(), n = file.size(archive))
  arrow::read_parquet(extract_metadata_payload(bytes))
}

expected_location <- function(offset, size, base) {
  sprintf("/vsisubfile/%.0f_%.0f,%s", offset, size, base)
}

# Writes `bytes` with a recomputed integrity hash, as a producer would.
write_rehashed <- function(bytes, path) {
  hash <- fnv1a_64(hash_input(bytes, index_size_from_lfh(bytes)))
  u32 <- function(x) as.raw(floor(x / 256^(0:3)) %% 256)
  bytes[44:51] <- c(u32(hash[["lo"]]), u32(hash[["hi"]]))
  writeBin(bytes, path)
  path
}

# Packs an edited manifest through the C writer alone, without the R
# manifest checks, as another producer could. The offsets stay right
# because the manifest is the last entry.
forge_archive <- function(tmp, edit, table = make_input_table(make_fixtures(tmp))) {
  res <- cozip::stage_metadata(table)
  pq <- file.path(tmp, "forged.parquet")
  arrow::write_parquet(edit(res$metadata), pq)
  out <- file.path(tmp, "forged.zip")
  .Call(cozip:::C_R_cozip_finalize, out,
        c(res$paths$name, "__metadata__"),
        c(res$paths$path, pq),
        c(file.size(res$paths$path), file.size(pq)),
        c(rep(FALSE, nrow(res$paths)), TRUE),
        1L)
  normalizePath(out)
}


describe("read", {

  it("returns every manifest column and a location", {
    tmp <- withr::local_tempdir()
    archive <- make_archive(tmp, make_input_table(make_fixtures(tmp)))
    result <- cozip::read(archive)
    stored <- stored_manifest(archive)

    expect_s3_class(result, "tbl_df")
    expect_identical(names(result), c(names(stored), "cozip:location"))
    expect_identical(result$name, c("a.txt", "b.bin"))
    expect_identical(
      result[["cozip:location"]],
      expected_location(result$offset, result$size, archive)
    )
  })

  it("addresses the stored bytes", {
    tmp <- withr::local_tempdir()
    fixtures <- make_fixtures(tmp)
    archive <- make_archive(tmp, make_input_table(fixtures))
    bytes <- readBin(archive, raw(), n = file.size(archive))
    result <- cozip::read(archive)
    payload <- function(i) bytes[(result$offset[i] + 1):(result$offset[i] + result$size[i])]

    expect_identical(payload(1), SMALL_CONTENT)
    expect_identical(payload(2), MEDIUM_CONTENT)
  })

  it("omits the location when asked", {
    tmp <- withr::local_tempdir()
    archive <- make_archive(tmp, make_input_table(make_fixtures(tmp)))
    expect_identical(
      names(cozip::read(archive, location = FALSE)),
      c("name", "offset", "size", "category")
    )
  })

  it("keeps the required columns first in a projection", {
    tmp <- withr::local_tempdir()
    fixtures <- make_fixtures(tmp)
    table <- make_input_table(fixtures)
    table[['sensor"name']] <- c("s1", "s2")
    archive <- make_archive(tmp, table)

    result <- cozip::read(
      archive,
      columns = c('sensor"name', "taco:location", "cozip:location", "category")
    )
    expect_identical(
      names(result),
      c("name", "offset", "size", "cozip:location", 'sensor"name', "category")
    )
    expect_identical(result[['sensor"name']], c("s1", "s2"))
    expect_identical(
      names(cozip::read(archive, columns = "category", location = FALSE)),
      c("name", "offset", "size", "category")
    )
  })

  it("ignores key=value directories in the archive path", {
    tmp <- withr::local_tempdir()
    archive <- make_archive(tmp, make_input_table(make_fixtures(tmp)))
    nested <- file.path(tmp, "year=2024")
    dir.create(nested)
    file.copy(archive, nested)
    expect_identical(
      names(cozip::read(file.path(nested, "out.zip"))),
      names(cozip::read(archive))
    )
  })

  it("drops stored location columns in any case", {
    tmp <- withr::local_tempdir()
    archive <- forge_archive(tmp, function(md) {
      md[["COZIP:LOCATION"]] <- arrow::Array$create(c("forged", "forged"))
      md[["Taco:Location"]] <- arrow::Array$create(c("forged", "forged"))
      md[["cozip:GDAL_VSI"]] <- arrow::Array$create(c("forged", "forged"))
      md
    })
    result <- cozip::read(archive)
    expect_identical(
      names(result),
      c("name", "offset", "size", "category", "cozip:location")
    )
    expect_identical(
      result[["cozip:location"]],
      expected_location(result$offset, result$size, archive)
    )
    expect_identical(
      names(cozip::read(archive, location = FALSE)),
      c("name", "offset", "size", "category")
    )
    expect_identical(
      names(cozip::read(archive, columns = c("COZIP:LOCATION", "category"),
                        location = FALSE)),
      c("name", "offset", "size", "category")
    )
  })

  it("rejects manifest columns that differ only in case", {
    tmp <- withr::local_tempdir()
    archive <- forge_archive(tmp, function(md) {
      md$Offset <- arrow::Array$create(c(7, 7))
      md
    })
    expect_error(
      cozip::read(archive),
      paste0(
        "cozip: the manifest has columns that differ only in case: ",
        shQuote("offset"), " and ", shQuote("Offset"),
        "; DuckDB, which decodes the manifest, ",
        "cannot tell them apart"
      ),
      fixed = TRUE
    )
  })

  it("requires the exact required column names", {
    tmp <- withr::local_tempdir()
    archive <- forge_archive(tmp, function(md) {
      arrow::arrow_table(name = md$name, offset = md$offset, SIZE = md$size)
    })
    expect_error(
      cozip::read(archive),
      paste0("cozip: the manifest lacks required column(s) ", shQuote("size")),
      fixed = TRUE
    )
  })

  it("skips nested children when listing columns", {
    tmp <- withr::local_tempdir()
    archive <- forge_archive(tmp, function(md) {
      md$bands <- arrow::Array$create(list(c(1, 2), 3))
      md$extent <- data.frame(name = c("x", "y"), size = c(1, 2))
      md
    })
    result <- cozip::read(archive)
    expect_identical(
      names(result),
      c("name", "offset", "size", "category", "bands", "extent", "cozip:location")
    )
    expect_identical(
      names(cozip::read(archive, columns = "extent")),
      c("name", "offset", "size", "cozip:location", "extent")
    )
  })

  it("rejects unknown columns, matched exactly", {
    tmp <- withr::local_tempdir()
    archive <- make_archive(tmp, make_input_table(make_fixtures(tmp)))
    expect_error(
      cozip::read(archive, columns = c("Category", "missing", "category")),
      paste0(
        "cozip: unknown column(s) ",
        paste(shQuote(c("Category", "missing")), collapse = ", "), "; ",
        "the manifest has ",
        paste(shQuote(c("name", "offset", "size", "category")), collapse = ", ")
      ),
      fixed = TRUE
    )
  })

  it("orders requested columns and drops duplicates", {
    tmp <- withr::local_tempdir()
    table <- make_input_table(make_fixtures(tmp))
    table$tag <- c("t1", "t2")
    archive <- make_archive(tmp, table)
    result <- cozip::read(
      archive,
      columns = c("tag", "Cozip:Location", "category", "tag", "size", "name",
                  "cozip:gdal_vsi")
    )
    expect_identical(
      names(result),
      c("name", "offset", "size", "cozip:location", "tag", "category")
    )
  })

  it("ignores a key=value directory named like a manifest column", {
    tmp <- withr::local_tempdir()
    archive <- make_archive(tmp, make_input_table(make_fixtures(tmp)))
    nested <- file.path(tmp, "size=7")
    dir.create(nested)
    file.copy(archive, nested)
    result <- cozip::read(file.path(nested, "out.zip"))
    expect_identical(result$size, cozip::read(archive)$size)
    expect_false(any(result$size == 7))
  })

  it("rejects the TACO profile with the Flat reader's message", {
    tmp <- withr::local_tempdir()
    archive <- make_archive(tmp, make_input_table(make_fixtures(tmp)))
    bytes <- readBin(archive, raw(), n = file.size(archive))
    bytes[INDEX_OFFSET + 7L] <- as.raw(2L)
    taco <- write_rehashed(bytes, file.path(tmp, "taco.zip"))

    expect_error(
      cozip::read(taco),
      paste0(
        "[cozip:PROFILE] read_flat needs a Flat-profile archive (profile=1). ",
        "Got profile=2 in: ", taco
      ),
      fixed = TRUE
    )
  })

  it("rejects a corrupt index", {
    tmp <- withr::local_tempdir()
    archive <- make_archive(tmp, make_input_table(make_fixtures(tmp)))
    bytes <- readBin(archive, raw(), n = file.size(archive))
    bytes[44] <- xor(bytes[44], as.raw(0xFF))
    corrupt <- file.path(tmp, "corrupt.zip")
    writeBin(bytes, corrupt)

    expect_error(cozip::read(corrupt), "cozip integrity hash mismatch", fixed = TRUE)
  })

  it("names the archive when its manifest is corrupt", {
    # Long names make the central directory larger than the hashed tail, so
    # the manifest footer can be damaged without breaking the hash.
    tmp <- withr::local_tempdir()
    paths <- file.path(tmp, sprintf("%d.bin", 1:300))
    for (path in paths) writeBin(charToRaw("x"), path)
    names <- sprintf("d/%s%04d.bin", strrep("n", 190), 1:300)
    archive <- file.path(tmp, "long.zip")
    cozip::create(archive, arrow::arrow_table(name = names, path = paths))
    bytes <- readBin(archive, raw(), n = file.size(archive))
    entry <- INDEX_OFFSET + 11L + 2L + 12L
    offset <- read_u64_le(bytes[(entry + 1L):(entry + 8L)])
    size <- read_u64_le(bytes[(entry + 9L):(entry + 16L)])
    expect_true(offset > 65536 && offset + size < length(bytes) - HASH_WINDOW)
    bytes[(offset + size - 3):(offset + size)] <- charToRaw("XXXX")
    writeBin(bytes, archive)

    message <- tryCatch(cozip::read(archive), error = conditionMessage)
    expect_true(grepl(paste0(archive, "!__metadata__"), message, fixed = TRUE))
    expect_false(grepl(".parquet", message, fixed = TRUE))
  })

  it("names a missing archive", {
    tmp <- withr::local_tempdir()
    missing <- file.path(tmp, "missing.zip")
    expect_error(cozip::read(missing), missing, fixed = TRUE)
  })

  it("stays out of the DuckDB home directory", {
    tmp <- withr::local_tempdir()
    archive <- make_archive(tmp, make_input_table(make_fixtures(tmp)))
    home <- file.path(tmp, "home")
    dir.create(home)
    withr::local_envvar(HOME = home)
    expect_identical(nrow(cozip::read(archive)), 2L)
    expect_false(dir.exists(file.path(home, ".duckdb")))
  })

  it("maps sources to GDAL paths", {
    base <- function(source) .Call(cozip:::C_R_cozip_location_base, source)
    expect_identical(base("data/out.zip"), "data/out.zip")
    expect_identical(base("https://host/out.zip"), "/vsicurl/https://host/out.zip")
    expect_identical(base("s3://bucket/out.zip"), "/vsis3/bucket/out.zip")
    expect_identical(base("az://container/out.zip"), "/vsiaz/container/out.zip")
    expect_identical(
      base("hf://datasets/owner/repo@v2/out.zip"),
      "/vsicurl/https://huggingface.co/datasets/owner/repo/resolve/v2/out.zip"
    )
    expect_error(base("hf://owner"), "cannot parse hf:// URL", fixed = TRUE)
  })
})
