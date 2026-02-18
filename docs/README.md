# Rhai Extension for DuckDB

---

![Ducks reading and following scripts](./images/ducks-evalexpr-rhai.jpg)

This `evalexpr_rhai` extension adds functions that allow the [Rhai](https://rhai.rs) language to be evaluated in [DuckDB's](https://www.duckdb.org) SQL statements.

## What is [Rhai](https://rhai.rs)?

[<image align="right" src="https://rhai.rs/book/images/logo/rhai-logo-transparent-colour-black.svg" width="200px"/>](https://rhai.rs)

A small, fast, easy-to-use scripting language and evaluation engine that integrates tightly with Rust.  It is very similar to Rust and JavaScript and uses dynamic typing.

You can learn more about Rhai by reading the [Rhai book](https://rhai.rs/book/).

## Why add this extension to DuckDB?

DuckDB offers a wide variety of SQL-based functions, but there are times when you want to write some code that is a bit more complicated than what SQL provides.

## Examples

```sql
load json;
load evalexpr_rhai;

-- Just a simple evaluation of an expression.

D select evalexpr_rhai('5+6').ok;
┌───────────────────────────┐
│ (evalexpr_rhai('5+6')).ok │
│           json            │
├───────────────────────────┤
│ 11                        │
└───────────────────────────
```

Expressions can either be passed in the statement itself or from a column in a database.  This means that you can evaluate expressions stored in columns for their result.

If a statement is passed as a constant expression it is compiled and cached for faster execution.

```sql
-- Setup a table that determines group members, the logic
-- for membership can be managed by an administrator
create table group_membership(group_name text, logic text);

insert into group_membership values
  ('managers', 'context.name == "George" || context.name == "Rusty"'),
  ('shift_leads', 'context.name == "John"'),
  ('employees', 'context.name == "Alex"');

-- Determine which groups the user is a member of
-- by evaluating the logic from the membership table.
select distinct group_name
from group_membership
where
evalexpr_rhai(logic, { name: 'John'}).ok
┌─────────────┐
│ group_name  │
│   varchar   │
├─────────────┤
│ shift_leads │
└─────────────┘
```

Scripting can be more advanced than expressions, you create functions.  It wouldn't be a scripting example without an example of a [Collatz](https://en.wikipedia.org/wiki/Collatz_conjecture) sequence.

```sql
-- Define a macro that calculates the length of
-- of the Collatz sequence from a starting value.
create macro collatz_series_length(n) as
evalexpr_rhai('
   fn collatz_series(n) {
       let count = 0;
       while n > 1 {
         count += 1;
         if n % 2 == 0 {
             n /= 2;
         } else {
             n = n * 3 + 1;
         }
       }
       return count
  }
  collatz_series(context.n)
', {'n': n});

-- Use the defined macro fucntion that calls the
-- rhai function.
select range as n,
collatz_series_length(range).ok::integer as length from range(1000, 2000) limit 5;
┌───────┬────────┐
│   n   │ length │
│ int64 │ int32  │
├───────┼────────┤
│  1000 │    111 │
│  1001 │    142 │
│  1002 │    111 │
│  1003 │     41 │
│  1004 │     67 │
└───────┴────────┘
```

### How can I make the data from the current row accessible to a Rhai expression?

You can just pass the entire row via the context.

```sql
create table employees (name text, state text, zip integer);
insert into employees values
  ('Jane', 'FL', 33139),
  ('John', 'NJ', 08520);

select evalexpr_rhai(
  '
  context.row.name + " is in " + context.row.state
  ',
  {
    row: employees
  }) as result from employees;
┌───────────────────────────────┐
│            result             │
│ union(ok json, error varchar) │
├───────────────────────────────┤
│ "Jane is in FL"               │
│ "John is in NJ"               │
└───────────────────────────────┘

-- What about augmenting the context, what is passed there?
-- just return the context.
select evalexpr_rhai('context',{
    row_data: employees,
    'fruit': 'banana'
  }) as result from employees;
┌────────────────────────────────────────────────────────────────────────┐
│                                 result                                 │
│                     union(ok json, error varchar)                      │
├────────────────────────────────────────────────────────────────────────┤
│ {"fruit":"banana","row_data":{"name":"Jane","state":"FL","zip":33139}} │
│ {"fruit":"banana","row_data":{"name":"John","state":"NJ","zip":8520}}  │
└────────────────────────────────────────────────────────────────────────┘
```

## API

`evalexpr_rhai(VARCHAR, JSON) -> UNION['ok': JSON, 'error': VARCHAR]`

The arguments in order are:

1. The [Rhai](https://rhai.rs) expression to evaluate.
2. Any context values that will be available to the Rhai expression by accessing a variable called `context`.

The return value is a [union](https://duckdb.org/docs/sql/data_types/union.html) type.  The union type is very similar to the [Result type from Rust](https://doc.rust-lang.org/std/result/).

If the Rhai expression was successfully evaluated the JSON result of the expression will be returned in the `ok` element of the union.  If there was an error evaluating the expression it will be returned in the `error` element of the expression.

## When would I use this?

You should use this when you want to have a simple way to write business logic in a database and have it evaluated reasonably quickly.

## Credits

1. This DuckDB extension utilizes and is named after the [`rhai`](https://crates.io/crates/rhai).

2. It also uses the [DuckDB Extension Template](https://github.com/duckdb/extension-template).

3. This extension uses [Corrosion](https://github.com/corrosion-rs/corrosion) to combine CMake with a Rust/Cargo build process.

4. I've gotten a lot of help from the generous DuckDB developer community.

### Build Architecture

For the DuckDB extension to call the Rust code a tool called `cbindgen` is used to write the C++ headers for the exposed Rust interface.

The headers can be updated by running `make rust_binding_headers`.

### Build steps
Now to build the extension, run:
```sh
make
```
The main binaries that will be built are:
```sh
./build/release/duckdb
./build/release/test/unittest
./build/release/extension/evalexpr_rhai/evalexpr_rhai.duckdb_extension
```
- `duckdb` is the binary for the duckdb shell with the extension code automatically loaded.
- `unittest` is the test runner of duckdb. Again, the extension is already linked into the binary.
- `evalexpr_rhai.duckdb_extension` is the loadable binary as it would be distributed.

## Running the extension
To run the extension code, simply start the shell with `./build/release/duckdb`.

Now we can use the features from the extension directly in DuckDB.

```
D select evalexpr_rhai('42');
┌───────────────────────────────┐
│      evalexpr_rhai('42')      │
│ union(ok json, error varchar) │
├───────────────────────────────┤
│ 42                            │
└───────────────────────────────┘
```

## Running the tests
Different tests can be created for DuckDB extensions. The primary way of testing DuckDB extensions should be the SQL tests in `./test/sql`. These SQL tests can be run using:
```sh
make test
```

### Installing the deployed binaries
To install your extension binaries from S3, you will need to do two things. Firstly, DuckDB should be launched with the
`allow_unsigned_extensions` option set to true. How to set this will depend on the client you're using. Some examples:

CLI:
```shell
duckdb -unsigned
```

Python:
```python
con = duckdb.connect(':memory:', config={'allow_unsigned_extensions' : 'true'})
```

NodeJS:
```js
db = new duckdb.Database(':memory:', {"allow_unsigned_extensions": "true"});
```

Secondly, you will need to set the repository endpoint in DuckDB to the HTTP url of your bucket + version of the extension
you want to install. To do this run the following SQL query in DuckDB:
```sql
SET custom_extension_repository='bucket.s3.us-east-1.amazonaws.com/evalexpr_rhai/latest';
```
Note that the `/latest` path will allow you to install the latest extension version available for your current version of
DuckDB. To specify a specific version, you can pass the version instead.

After running these steps, you can install and load your extension using the regular INSTALL/LOAD commands in DuckDB:
```sql
INSTALL evalexpr_rhai
LOAD evalexpr_rhai
```
