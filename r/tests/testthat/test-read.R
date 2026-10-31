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
