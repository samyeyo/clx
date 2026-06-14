# Contributing to clx

First of all, thank you for your interest in improving clx.

Contributions of all kinds are welcome, including:

* Bug reports
* Compatibility reports
* Edge-case test cases
* Documentation improvements
* Performance improvements
* New examples
* Pull requests

## Reporting bugs

Before opening an issue:

1. Check existing issues.
2. Verify the problem on the latest version.
3. Reduce the problem to the smallest possible Lua source file.

A good bug report should include:

* Operating system
* Compiler used (Clang, GCC, MSVC)
* clx version
* Minimal Lua source reproducing the problem
* Expected behavior
* Actual behavior

## Compatibility issues

clx targets Lua 5.5 compatibility.

If you find a Lua program that behaves differently from the reference implementation, please open an issue and provide:

* The Lua source file
* Expected output
* Actual output

Small reproducing examples are particularly valuable.

## Edge cases

Edge cases are extremely useful for compiler development.

Examples include:

* Parser corner cases
* Scope resolution issues
* Closure behavior
* Metatable interactions
* Coroutine behavior
* Multiple return value handling

Minimal reproductions are preferred.

## Pull requests

Before submitting a pull request:

* Ensure the code builds successfully.
* Add tests when applicable.
* Run the test suite before submitting changes.
* Don't submit a PR that breaks the test suite
* Update documentation when applicable.
* Keep changes focused and self-contained.

## Coding style

General guidelines:

* Prefer clear code over clever code.
* Keep runtime code portable.
* Avoid compiler-specific extensions unless necessary.
* Preserve compatibility across supported platforms.

## Testing

Run the test suite before submitting changes:

```bash
cd tests
./runtests.sh
```

Please add new tests whenever fixing a bug.

Regression tests are especially appreciated.

## Documentation

Documentation improvements are welcome.

If a feature, behavior, limitation, or optimization is not obvious, consider documenting it.

## Community

Please remain respectful and constructive when discussing issues and pull requests.

The goal is to build a reliable and maintainable Lua compiler that benefits the wider Lua community.

Thank you for contributing to clx.
