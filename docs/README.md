<p align="center">
  <a href="https://query.farm">
    <picture>
      <source media="(prefers-color-scheme: dark)" srcset="https://query.farm/media-kit/logo/wordmark-dark.svg">
      <img alt="Query.Farm" src="https://query.farm/media-kit/logo/wordmark-light.svg" height="64">
    </picture>
  </a>
</p>

# Rhai Extension for DuckDB

[![DuckDB](https://img.shields.io/badge/DuckDB-community_extension-fdf1e0?logo=duckdb&logoColor=fff000)](https://duckdb.org/community_extensions/extensions/evalexpr_rhai.html)
[![v1.5 build](https://github.com/Query-farm/evalexpr_rhai/actions/workflows/MainDistributionPipeline.yml/badge.svg?branch=v1.5)](https://github.com/Query-farm/evalexpr_rhai/actions/workflows/MainDistributionPipeline.yml?query=branch%3Av1.5)

This extension, `evalexpr_rhai`, adds functions that allow the [Rhai](https://rhai.rs) scripting language to be evaluated within [DuckDB](https://duckdb.org) SQL statements.

## Documentation

Full documentation, including installation, usage, the function reference, and cookbook examples, is available at:

**[https://query.farm/products/extensions/evalexpr_rhai](https://query.farm/products/extensions/evalexpr_rhai)**

## Installation

```sql
INSTALL evalexpr_rhai FROM community;
LOAD evalexpr_rhai;
```

## Development

For instructions on building the extension from source and running its tests, see [BUILDING.md](BUILDING.md).
