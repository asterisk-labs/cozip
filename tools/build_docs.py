"""Build the GitHub Pages artifact from docs/ and render SPEC.md into it.

With --sync-spec, write the rendered specification into docs/spec.html too, so
the page reads the same when opened from the source tree.
"""

from __future__ import annotations

import argparse
import html
import re
import shutil
import sys
import unicodedata
from html.parser import HTMLParser
from pathlib import Path
from urllib.parse import unquote, urlsplit

from markdown_it import MarkdownIt

ROOT = Path(__file__).resolve().parents[1]
DOCS_SOURCE = ROOT / "docs"
SPEC_SOURCE = ROOT / "SPEC.md"
SPEC_PAGE = "spec.html"

# Files in docs/ that are notes for the repository, not pages of the site.
NOT_PUBLISHED = ("README.md", "requirements.txt", "taco-writer-internal.md")


class DocumentParser(HTMLParser):
    def __init__(self) -> None:
        super().__init__(convert_charrefs=True)
        self.ids: set[str] = set()
        self.references: list[str] = []

    def handle_starttag(self, _tag: str, attrs: list[tuple[str, str | None]]) -> None:
        values = dict(attrs)
        if values.get("id"):
            self.ids.add(values["id"] or "")
        for name in ("href", "src"):
            if values.get(name):
                self.references.append(values[name] or "")

    handle_startendtag = handle_starttag


def slugify(value: str) -> str:
    normalized = unicodedata.normalize("NFKD", value).encode("ascii", "ignore").decode()
    normalized = normalized.lower().strip()
    normalized = re.sub(r"[^a-z0-9 _-]", "", normalized)
    return re.sub(r"[ -]+", "-", normalized).strip("-") or "section"


def inline_text(token) -> str:
    if not token.children:
        return token.content
    return "".join(
        child.content
        for child in token.children
        if child.type in {"text", "code_inline", "html_inline"}
    )


def spec_facts(tokens) -> tuple[str, list]:
    """Turn the lines between the title and the first rule into the facts line.

    SPEC.md opens with lines such as `**Version** 0.1.0`. Each becomes one item
    of the facts list, `Version 0.1.0`, and everything up to the first `---`
    is consumed.
    """
    end = next((i for i, t in enumerate(tokens) if t.type == "hr"), None)
    if end is None:
        raise ValueError("SPEC.md must end its front matter with a --- rule")
    facts: list[str] = []
    for token in tokens[:end]:
        if token.type != "inline":
            continue
        for line in token.content.splitlines():
            match = re.fullmatch(r"\*\*(.+?)\*\*\s+(.+?)\s*", line.strip())
            if match:
                facts.append(f"{match.group(1)} {match.group(2)}")
    if not facts:
        raise ValueError("SPEC.md must list its version and status under the title")
    items = "\n".join(f"<li>{html.escape(fact)}</li>" for fact in facts)
    return f'<ul class="facts">\n{items}\n</ul>\n', tokens[end + 1:]


def render_spec() -> dict[str, str]:
    """Render SPEC.md for the reading layout of docs/spec.html.

    The page has its own title, so the document heading is dropped, and the
    lines under it become the facts line. Sections, lists and tables take the
    classes the other docs pages use.
    """
    md = MarkdownIt("commonmark", {"html": False, "linkify": False})
    md.enable("table")
    md.enable("strikethrough")
    tokens = md.parse(SPEC_SOURCE.read_text(encoding="utf-8"))

    if tokens[0].type != "heading_open" or tokens[0].tag != "h1":
        raise ValueError("SPEC.md must start with its title")
    facts, tokens = spec_facts(tokens[3:])

    headings: list[tuple[int, str, str]] = []
    used_slugs: dict[str, int] = {}
    anchors: set[str] = set()
    local_anchor_links: list[str] = []

    for index, token in enumerate(tokens):
        if token.type in {"bullet_list_open", "ordered_list_open"}:
            token.attrSet("class", "list")
        elif token.type == "table_open":
            token.attrSet("class", "spec")
        if token.type == "heading_open":
            if token.tag == "h2":
                token.attrSet("class", "part")
            title = inline_text(tokens[index + 1])
            base = slugify(title)
            seen = used_slugs.get(base, 0)
            used_slugs[base] = seen + 1
            anchor = base if seen == 0 else f"{base}-{seen}"
            token.attrSet("id", anchor)
            anchors.add(anchor)
            headings.append((int(token.tag[1]), title, anchor))

        if token.type != "inline" or not token.children:
            continue
        for child in token.children:
            if child.type != "link_open":
                continue
            href = child.attrGet("href") or ""
            if href.startswith("#"):
                local_anchor_links.append(href[1:])

    missing = sorted(set(local_anchor_links) - anchors)
    if missing:
        raise ValueError(f"SPEC.md links to missing headings: {', '.join(missing)}")

    items = [
        f'<li><a href="#{html.escape(anchor)}">{html.escape(title)}</a></li>'
        for level, title, anchor in headings
        if level == 2
    ]
    return {
        "FACTS": facts,
        "BODY": md.renderer.render(tokens, md.options, {}),
        "TOC": "\n".join(items),
    }


def replace_generated_region(document: str, name: str, content: str) -> str:
    start = f"<!-- COZIP_SPEC_{name}_START -->"
    end = f"<!-- COZIP_SPEC_{name}_END -->"
    start_index = document.find(start)
    end_index = document.find(end)
    if start_index < 0 or end_index < 0 or end_index < start_index:
        raise ValueError(f"docs/{SPEC_PAGE} is missing the {name.lower()} markers")
    head = document[: start_index + len(start)]
    return f"{head}\n{content.rstrip()}\n{document[end_index:]}"


def write_spec(page: Path) -> None:
    document = page.read_text(encoding="utf-8")
    for name, content in render_spec().items():
        document = replace_generated_region(document, name, content)
    page.write_text(document, encoding="utf-8")


def assert_local_assets(output: Path) -> None:
    required = (
        output / "index.html",
        output / SPEC_PAGE,
        output / "api.html",
        output / "api-python.html",
        output / "api-r.html",
        output / "api-julia.html",
        output / "api-c.html",
        output / "api-js.html",
        output / "playground.html",
        output / "deck" / "index.html",
        output / "assets" / "css" / "reading.css",
        output / "assets" / "js" / "code.js",
        output / "assets" / "js" / "read.js",
        output / "assets" / "svg" / "cozip-mark.svg",
    )
    missing = [str(path.relative_to(output)) for path in required if not path.is_file()]
    if missing:
        raise FileNotFoundError(f"docs build is missing: {', '.join(missing)}")


def assert_local_links(output: Path) -> None:
    root = output.resolve()
    documents: dict[Path, DocumentParser] = {}
    for page in output.rglob("*.html"):
        parser = DocumentParser()
        parser.feed(page.read_text(encoding="utf-8"))
        documents[page.resolve()] = parser

    errors: list[str] = []
    for page, parser in documents.items():
        for reference in parser.references:
            parsed = urlsplit(reference)
            if parsed.scheme or parsed.netloc or reference.startswith(("mailto:", "data:")):
                continue
            path = page if not parsed.path else (page.parent / unquote(parsed.path)).resolve()
            if path.is_dir():
                path /= "index.html"
            if not path.is_relative_to(root):
                errors.append(f"{page.relative_to(root)} escapes the docs site: {reference}")
                continue
            if not path.is_file():
                errors.append(f"{page.relative_to(root)} has missing target: {reference}")
                continue
            if parsed.fragment and path.suffix == ".html":
                target = documents.get(path.resolve())
                if target is None or unquote(parsed.fragment) not in target.ids:
                    errors.append(f"{page.relative_to(root)} has missing anchor: {reference}")

    if errors:
        raise ValueError("; ".join(errors))


def prepare_output(output: Path, *, clean: bool) -> None:
    if output.exists() and not output.is_dir():
        raise ValueError(f"output path is not a directory: {output}")
    if not output.exists():
        return
    if any(output.iterdir()):
        if not clean:
            raise FileExistsError(f"output directory is not empty: {output}")
        if output.name != "_site":
            raise ValueError(f"refusing to clean output not named _site: {output}")
        shutil.rmtree(output)
        return
    output.rmdir()


def build(output: Path, *, clean: bool = False) -> None:
    prepare_output(output, clean=clean)
    shutil.copytree(DOCS_SOURCE, output, ignore=shutil.ignore_patterns(*NOT_PUBLISHED, ".DS_Store"))
    (output / ".nojekyll").touch()
    write_spec(output / SPEC_PAGE)
    assert_local_assets(output)
    assert_local_links(output)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "_site")
    parser.add_argument("--clean", action="store_true", help="replace an existing _site")
    parser.add_argument(
        "--sync-spec",
        action="store_true",
        help=f"also write the rendered SPEC.md into docs/{SPEC_PAGE}",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        if args.sync_spec:
            write_spec(DOCS_SOURCE / SPEC_PAGE)
        build(args.output.resolve(), clean=args.clean)
    except (FileExistsError, FileNotFoundError, ValueError) as exc:
        print(f"docs build failed: {exc}", file=sys.stderr)
        return 1
    print(f"built the docs website in {args.output.resolve()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
