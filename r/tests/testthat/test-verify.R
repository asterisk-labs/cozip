# cozip::verify checks the index and the integrity hash of any profile.

u32_le <- function(x) as.raw(c(x %% 256, (x %/% 256) %% 256,
                               (x %/% 65536) %% 256, (x %/% 16777216) %% 256))

rehash <- function(data) {
  h <- fnv1a_64(hash_input(data, index_size_from_lfh(data)))
  data[44:51] <- c(u32_le(h[["lo"]]), u32_le(h[["hi"]]))
  data
}

patched <- function(archive, tmp, patch, rehashed) {
  data <- patch(readBin(archive, raw(), file.size(archive)))
  if (rehashed) data <- rehash(data)
  out <- file.path(tmp, "patched.zip")
  writeBin(data, out)
  out
}

describe("verify", {
  it("returns NULL invisibly for an intact archive", {
    tmp <- withr::local_tempdir()
    archive <- make_archive(tmp, make_input_table(make_fixtures(tmp)))
    expect_invisible(res <- cozip::verify(archive))
    expect_null(res)
  })

  it("accepts any profile value", {
    tmp <- withr::local_tempdir()
    archive <- make_archive(tmp, make_input_table(make_fixtures(tmp)))
    for (profile in c(0L, 2L, 7L)) {
      bad <- patched(archive, tmp, function(d) { d[58] <- as.raw(profile); d }, TRUE)
      expect_null(cozip::verify(bad))
    }
  })

  it("names the rule a broken archive breaks", {
    tmp <- withr::local_tempdir()
    archive <- make_archive(tmp, make_input_table(make_fixtures(tmp)))
    flip <- function(i) function(d) { d[i] <- xor(d[i], as.raw(1)); d }
    expect_error(cozip::verify(patched(archive, tmp, flip(44), FALSE)),
                 "[cozip:HASH_MISMATCH] cozip integrity hash mismatch", fixed = TRUE)
    end_comment <- function(d) { d[length(d) - 1L] <- as.raw(1); d }
    expect_error(cozip::verify(patched(archive, tmp, end_comment, TRUE)),
                 "[cozip:INVALID_ZIP_STRUCTURE]", fixed = TRUE)
    absolute <- function(d) { d[65] <- charToRaw("/"); d }
    expect_error(cozip::verify(patched(archive, tmp, absolute, TRUE)),
                 "[cozip:INVALID_NAME] cozip index entry 0 name must be relative",
                 fixed = TRUE)
    small <- file.path(tmp, "small.zip")
    writeBin(readBin(archive, raw(), INDEX_OFFSET + HASH_WINDOW - 1L), small)
    expect_error(cozip::verify(small), "[cozip:ARCHIVE_TOO_SMALL]", fixed = TRUE)
    expect_error(cozip::verify(file.path(tmp, "missing.zip")), "missing.zip",
                 fixed = TRUE)
  })

  it("checks the source before reading", {
    expect_error(cozip::verify(c("a.zip", "b.zip")), "single non-empty string")
    expect_error(cozip::verify(NA_character_), "single non-empty string")
    expect_error(cozip::verify(""), "single non-empty string")
    expect_error(cozip::verify("niño.zip"), "non-ASCII")
  })
})
