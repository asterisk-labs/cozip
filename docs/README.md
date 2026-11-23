# cozip site

`docs/` is the source of [asterisk.coop/cozip](https://asterisk.coop/cozip/).
Preview it locally with:

```bash
python -m http.server 8000 --directory docs
```

## Layout

The landing page has the interactive read figure. The API, specification and
playground share the layout in `assets/css/reading.css`. Each fact lives on one
page: how cozip works and the rules every binding shares in `api.html`, only what
differs by language on the language pages, and the format in `spec.html`. Link to
a section instead of repeating it.

```
index.html            landing page and interactive read figure
api.html              how cozip works and what every binding shares
api-*.html            one page per language
spec.html             rendered SPEC.md
playground.html       browser viewer for public archives
deck/                 presentation
assets/               styles, scripts, figures and playground data
```

The figures on `api.html`, the landing figure and the deck use a reference archive of a
million files. `python tools/reference_archive.py OUT_DIR` rebuilds it and prints every
size and timing they quote. Runnable code examples use `cities.zip` from
[cozip-api-fixtures](https://huggingface.co/datasets/asterisk-labs/cozip-api-fixtures)
at `v0.1.0`, which anyone can fetch.

`spec.html` renders `SPEC.md` during the build. After changing the spec, run
`python3 tools/build_docs.py --sync-spec --output _site --clean` to update the
source page for direct browsing too.

`taco-writer-internal.md` documents the private TACO writer contract for the
repository and is not published.

## Build

Install the pinned Markdown renderer and build the site:

```bash
python -m pip install -r docs/requirements.txt
make docs
python -m http.server 8000 --directory _site
```

The build checks local links and anchors. `_site/` is ignored by Git. GitHub
Actions builds pull requests and deploys changes pushed to `main`.
