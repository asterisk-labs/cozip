using Pkg.Artifacts
using Downloads
using SHA

const REPO = "asterisk-labs/taco"
const VERSION = if !isempty(ARGS)
    strip(ARGS[1])
else
    strip(read(joinpath(@__DIR__, "..", "VERSION"), String))
end

const PLATFORMS = [
    (
        kv = ["arch = \"aarch64\"", "os = \"linux\"", "libc = \"glibc\""],
        asset = "libcozip-$(VERSION)-linux-aarch64.tar.gz",
        kind  = :tar,
    ),
    (
        kv = ["arch = \"x86_64\"", "os = \"linux\"", "libc = \"glibc\""],
        asset = "libcozip-$(VERSION)-linux-x86_64.tar.gz",
        kind  = :tar,
    ),
    (
        kv = ["arch = \"x86_64\"", "os = \"macos\""],
        asset = "libcozip-$(VERSION)-macos-universal.tar.gz",
        kind  = :tar,
    ),
    (
        kv = ["arch = \"aarch64\"", "os = \"macos\""],
        asset = "libcozip-$(VERSION)-macos-universal.tar.gz",
        kind  = :tar,
    ),
    (
        kv = ["arch = \"x86_64\"", "os = \"windows\""],
        asset = "libcozip-$(VERSION)-windows-x86_64.tar.gz",
        kind  = :tar,
    ),
]


function fetch_and_hash(asset::String, kind::Symbol)
    url = "https://github.com/$REPO/releases/download/v$VERSION/$asset"
    @info "Fetching" url

    archive = Downloads.download(url)
    sha256_hex = bytes2hex(open(SHA.sha256, archive))

    tree = create_artifact() do dir
        cmd = if kind === :tar
            `tar -xzf $archive -C $dir`
        elseif kind === :zip
            `unzip -q $archive -d $dir`
        else
            error("unknown archive kind: $kind")
        end
        run(cmd)
    end

    return (sha256 = sha256_hex, tree = string(tree), url = url)
end


function main()
    for plat in PLATFORMS
        info = fetch_and_hash(plat.asset, plat.kind)

        println("[[cozip]]")
        for line in plat.kv
            println(line)
        end
        println("git-tree-sha1 = \"$(info.tree)\"")
        println("lazy = true")
        println()
        println("    [[cozip.download]]")
        println("    url = \"$(info.url)\"")
        println("    sha256 = \"$(info.sha256)\"")
        println()
    end
end


main()