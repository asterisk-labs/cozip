# karu

*karu is the Quechua word for far.*

[![CI](https://github.com/asterisk-labs/karu/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/asterisk-labs/karu/actions/workflows/ci.yml)
[![Coverage](https://codecov.io/gh/asterisk-labs/karu/graph/badge.svg?branch=main)](https://codecov.io/gh/asterisk-labs/karu)
![C++23](https://img.shields.io/badge/C%2B%2B-23-4E04EB.svg)
![Linux, macOS, and Windows](https://img.shields.io/badge/platform-Linux%20%7C%20macOS%20%7C%20Windows-4E04EB.svg)
[![MIT license](https://img.shields.io/badge/license-MIT-2b8a3e.svg)](#license)

Karu is a C++23 library for reading exact byte ranges from local files, HTTP,
S3, Google Cloud Storage, Azure Storage, Hugging Face, and
[Source Cooperative](https://source.coop). It accepts URI and VSI paths while
keeping reads positional and stateless.

Karu's VSI path syntax is strongly inspired by GDAL VSI.

## Quick start

This reads the first 4 KiB of a public
[Rumi fixture on Hugging Face](https://huggingface.co/datasets/asterisk-labs/rumi-api-fixtures):

```cpp
#include <karu/karu.hpp>

auto config = karu::Config::from_environment().value();
auto client = karu::Client::create(config).value();
auto object = karu::Object::parse(
    "hf://datasets/asterisk-labs/rumi-api-fixtures/data/s2-00-tile.rumi"
).value();

auto bytes = client.read(object, 0, 4096).value();
```

For private Hugging Face repositories, run `hf auth login`. Karu reads the
token created by the Hugging Face CLI.

## Paths

| Backend | URI | VSI path |
|---|---|---|
| HTTP | `https://host/object` | `/vsicurl/https://host/object` |
| AWS S3 | `s3://bucket/key` | `/vsis3/bucket/key` |
| Google Cloud Storage | `gs://bucket/key` | `/vsigs/bucket/key` |
| Azure Blob | `az://container/key` | `/vsiaz/container/key` |
| Azure Data Lake | `abfs://container/key` | `/vsiadls/container/key` |
| Hugging Face | `hf://datasets/org/repo/path` | `/vsihf/datasets/org/repo/path` |
| Source Cooperative | `source://account/product/key` | `/vsisource/account/product/key` |
|---|---|---|

## Configuration

Options can apply to the whole client or to the longest matching path prefix:

```cpp
auto config = karu::Config::from_environment().value();
config.set("AWS_REGION", "us-west-2").value();
config.set_path("/vsis3/public/", "AWS_NO_SIGN_REQUEST", "YES").value();
config.set_path("/vsis3/private/", "AWS_PROFILE", "research").value();
```

Karu supports standard credential sources for AWS, GCS, Azure, Hugging Face,
and Source Cooperative. See [`CONFIGURATION.md`](CONFIGURATION.md) for the
complete option and credential reference.

## Build

Karu requires CMake 3.21, a C++23 compiler, libcurl 7.83 or newer, OpenSSL 3,
and threads.

```sh
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DKARU_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

## AI agent skill

Install the [Karu skill](https://github.com/asterisk-labs/karu/blob/main/.claude/skills/karu/SKILL.md)
so coding agents know its path grammar, batch semantics, configuration and credential
rules.

```sh
npx skills add asterisk-labs/karu
```

## License

MIT. See [`LICENSE`](LICENSE).

<div align="center">
  <br>
  Made with ♥ by
  <br><br>
  <a href="https://asterisk.coop">
    <img src="https://raw.githubusercontent.com/asterisk-labs/cozip/refs/heads/main/images/asterisk_logo.svg" alt="Asterisk Labs" width="320"/>
  </a>
</div>
