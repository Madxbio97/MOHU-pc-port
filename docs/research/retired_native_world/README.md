# Retired native-world projection prototype

This directory preserves the first static-world projection experiment for
reference only. It is intentionally excluded from all production and test
targets.

The prototype reconstructed a frame after packet generation and accepted it
through a fixed one-pixel screen-space witness. That contract cannot prove
camera-space depth or associate a projection with the exact guest render
generation, so it is unsuitable for PGXP or perspective-correct rendering.

The deterministic TSP parser, level loader, and mesh builder remain active in
`src/mohu` and covered by tests. Any future native renderer must consume an
immutable camera/geometry generation captured at the producer boundary and use
explicit provenance instead of a pixel-distance heuristic.
