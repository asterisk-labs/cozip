# ---- Fixture content (deterministic recipe, shared with Python) ----

SMALL_CONTENT  <- charToRaw(strrep("hello cozip\n", 8))    #    96 bytes
MEDIUM_CONTENT <- rep(as.raw(0:255), 160)                  # 40960 bytes


# ---- Format constants (cozip 1.0 SPEC) ----

INDEX_OFFSET      <- 51L
HASH_WINDOW       <- 32768L
ZIP_LFH_SIG       <- as.raw(c(0x50, 0x4B, 0x03, 0x04))     # "PK\x03\x04"
INDEX_NAME        <- charToRaw("__cozip__")
HASH_BLOCK_HEADER <- as.raw(c(0x0c, 0xca, 0x08, 0x00))
INDEX_MAGIC       <- charToRaw("CZIP")
INDEX_VERSION     <- as.raw(c(0x01, 0x00))
PROFILE_FLAT      <- 1L


# ---- Little-endian byte readers ----
# All return double. Fine for archive offsets < 2^53.

read_u16_le <- function(b) {
  bi <- as.integer(b)
  bi[1] + bi[2] * 256L
}

read_u32_le <- function(b) {
  bn <- as.numeric(as.integer(b))
  bn[1] + bn[2] * 256 + bn[3] * 65536 + bn[4] * 16777216
}

read_u64_le <- function(b) {
  bn <- as.numeric(as.integer(b))
  Reduce(function(acc, x) acc * 256 + x, rev(bn))
}


# ---- FNV-1a 64 ----

fnv1a_64 <- function(data) {
  hi <- 0xCBF29CE4    # 3421674724 — high 32 of offset basis
  lo <- 0x84222325    # 2216829733 — low  32 of offset basis
  ph <- 0x00000100    # 256        — high 32 of prime (prime = 2^40 + 435)
  pl <- 0x000001B3    # 435        — low  32 of prime
  TWO32 <- 4294967296

  for (byte in as.integer(data)) {
    lo_byte <- lo %% 256
    lo <- lo - lo_byte + bitwXor(as.integer(lo_byte), byte)
    lo_pl <- lo * pl
    cross <- lo * ph + hi * pl

    new_lo <- lo_pl %% TWO32
    carry  <- (lo_pl - new_lo) / TWO32
    hi <- (cross + carry) %% TWO32
    lo <- new_lo
  }
  c(hi = hi, lo = lo)
}


# ---- Hash input region ----

hash_input <- function(archive, index_size) {
  archive_size <- length(archive)
  suffix_start <- archive_size - HASH_WINDOW
  index_end    <- INDEX_OFFSET + index_size

  if (index_end <= suffix_start) {
    c(archive[(INDEX_OFFSET + 1L):index_end],
      archive[(suffix_start  + 1L):archive_size])
  } else {
    archive[(INDEX_OFFSET + 1L):archive_size]
  }
}


# ---- Index payload parser ----
# Returns a named list keyed by entry name; each value is
# c(offset = ..., size = ...).

parse_index <- function(payload) {
  stopifnot(identical(payload[1:4], INDEX_MAGIC))

  n   <- read_u32_le(payload[8:11])
  cur <- 11L  # 0-based byte cursor

  name_lens <- integer(n)
  for (i in seq_len(n)) {
    name_lens[i] <- read_u16_le(payload[(cur + 1L):(cur + 2L)])
    cur <- cur + 2L
  }

  names_v <- character(n)
  for (i in seq_len(n)) {
    nl <- name_lens[i]
    names_v[i] <- rawToChar(payload[(cur + 1L):(cur + nl)])
    cur <- cur + nl
  }

  offsets <- numeric(n)
  for (i in seq_len(n)) {
    offsets[i] <- read_u64_le(payload[(cur + 1L):(cur + 8L)])
    cur <- cur + 8L
  }

  sizes <- numeric(n)
  for (i in seq_len(n)) {
    sizes[i] <- read_u64_le(payload[(cur + 1L):(cur + 8L)])
    cur <- cur + 8L
  }

  setNames(
    lapply(seq_len(n), function(i) c(offset = offsets[i], size = sizes[i])),
    names_v
  )
}


# ---- LFH-derived index size ----

index_size_from_lfh <- function(archive) {
  read_u32_le(archive[19:22])
}


# ---- Extract __metadata__ payload bytes ----

extract_metadata_payload <- function(archive) {
  idx_size <- index_size_from_lfh(archive)
  payload  <- archive[(INDEX_OFFSET + 1L):(INDEX_OFFSET + idx_size)]
  meta     <- parse_index(payload)[["__metadata__"]]
  off <- meta[["offset"]]
  sz  <- meta[["size"]]
  archive[(off + 1L):(off + sz)]
}


# ---- Quick spec-invariant sanity check ----

assert_valid_cozip <- function(data) {
  testthat::expect_gte(length(data), HASH_WINDOW + INDEX_OFFSET)
  testthat::expect_identical(data[1:4],   ZIP_LFH_SIG)
  testthat::expect_identical(data[31:39], INDEX_NAME)
  testthat::expect_identical(data[52:55], INDEX_MAGIC)

  expected <- fnv1a_64(hash_input(data, index_size_from_lfh(data)))
  stored_b <- data[44:51]
  stored   <- c(
    hi = read_u32_le(stored_b[5:8]),
    lo = read_u32_le(stored_b[1:4])
  )
  testthat::expect_identical(stored, expected)
}


# ---- Fixture constructors (mirror @pytest.fixture) ----
# Each takes tmp_path explicitly; tests use withr::local_tempdir()
# at the top of test_that() and pass it in.

make_fixtures <- function(tmp_path) {
  small  <- file.path(tmp_path, "small.txt")
  medium <- file.path(tmp_path, "medium.bin")
  writeBin(SMALL_CONTENT,  small)
  writeBin(MEDIUM_CONTENT, medium)
  list(small = small, medium = medium)
}

make_input_table <- function(fixtures) {
  arrow::arrow_table(
    name     = c("a.txt", "b.bin"),
    path     = c(fixtures$small, fixtures$medium),
    category = c("text", "binary")
  )
}

make_paths_arg <- function(input_table) {
  data.frame(
    name = as.character(input_table[["name"]]),
    path = as.character(input_table[["path"]]),
    stringsAsFactors = FALSE
  )
}

make_archive <- function(tmp_path, input_table) {
  out <- file.path(tmp_path, "out.zip")
  cozip::create(out, input_table)
  out
}