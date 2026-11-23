# Natural Earth basemap

`natural-earth-countries.js` is derived from Natural Earth's 1:110m
Admin 0 Countries dataset. Natural Earth data is in the public domain; see
https://www.naturalearthdata.com/about/terms-of-use/.

The browser copy exposes one local GeoJSON object, keeps geometry only, and
rounds coordinates to four decimal places to reduce the download size. It is a
script instead of a fetched `.geojson` file so the playground also works when
opened directly from disk.
