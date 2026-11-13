# Contributing

## Code of Conduct

This project and everyone participating in it is governed by a [Code of Conduct](CODE_OF_CONDUCT.md). By participating, you are expected to uphold this code. Please report unacceptable behavior to [quack@duckdb.org](mailto:quack@duckdb.org).

## **Did you find a bug?**

* **Ensure the bug was not already reported** by searching on GitHub under [Issues](https://github.com/duckdb/duckdb/issues).
* If you're unable to find an open issue addressing the problem, [open a new one](https://github.com/duckdb/duckdb/issues/new/choose). Be sure to include a **title and clear description**, as much relevant information as possible, and a **code sample** or an **executable test case** demonstrating the expected behavior that is not occurring.

## **Did you write a patch that fixes a bug?**

* Great!
* If possible, add a unit test case to make sure the issue does not occur again.
* Make sure you run the code formatter (`make format-fix`).
* Open a new GitHub pull request with the patch.
* Ensure the PR description clearly describes the problem and solution. Include the relevant issue number if applicable.

## Outside Contributors

* Discuss your intended changes with the core team on Github
* Announce that you are working or want to work on a specific issue
* Avoid large pull requests - they are much less likely to be merged as they are incredibly hard to review

## Pull Requests

* Do not commit/push directly to the main branch. Instead, create a fork and file a pull request.
* When maintaining a branch, merge frequently with the main.
* When maintaining a branch, submit pull requests to the main frequently.
* If you are working on a bigger issue try to split it up into several smaller issues.
* Please do not open "Draft" pull requests and ensure that you run CI on your fork + branch first before opening a PR (see CI section below). GH actions is free for public forks of open source projects.
* We reserve full and final discretion over whether or not we will merge a pull request. Adhering to these guidelines is not a complete guarantee that your pull request will be merged.
* Please refrain from opening pull requests to fix minor typos.

## CI for pull requests

* Pull requests will need to pass all continuous integration checks before merging.
* The CI worklow running inside the main repository must be approved on every push by new contributors (until a commit or PR by them is merged). It is recommended to run the same workflow in your own fork (without needing approval) to ensure everything works before opening your PR. See below under "Testing" how to enable this.
* When all CI checks look good on your fork, you can open a PR to run the CI on the main DuckDB repo. Submitting changes to an open pull request will move it to 'Draft' state. In that case you can mark it as "Ready for Review" once you've applied all fixes and it passes in your fork to run the public CI again ('ready for review', via the Web UI button on the bottom right).
* Note that occasionally CI failures may be unrelated. You should check whether it's related to your changes (because if it is, that means your changes are breaking something). Otherwise, you should 1) remember to merge with main frequently and run make format-fix (sometimes you need to run generate-files) 2) check if other PR CI's are failing on the same tests (that's usually a giveaway that it's a temporary problem with the CI) and generally 3) investigate that there is no overlap between your changes and the breaking CI.

## Nightly CI

* Packages creation and long running tests will be performed during a nightly run
* On your fork you can trigger long running tests (NightlyTests.yml) for any branch following information from https://docs.github.com/en/actions/using-workflows/manually-running-a-workflow#running-a-workflow

## Building

* To build the project, run `make`.
* To build the project for debugging, run `make debug`.
* For parallel builds, you can use the [Ninja](https://ninja-build.org/) build system: `GEN=ninja make`.
  * The default number of parallel processes can lock up the system depending on the CPU-to-memory ratio. If this happens, restrict the maximum number of build processes: `CMAKE_BUILD_PARALLEL_LEVEL=4 GEN=ninja make`.
  * Without using Ninja, build times can still be reduced by setting `CMAKE_BUILD_PARALLEL_LEVEL=$(nproc)`.
* To speed up rebuilds, install [ccache](https://ccache.dev/). The build system will automatically detect and use it if available.

## Testing

* Unit tests can be written either using the sqllogictest framework (`.test` files) or in C++ directly. We **strongly** prefer tests to be written using the sqllogictest framework. Only write tests in C++ if you absolutely need to (e.g. when testing concurrent connections or other exotic behavior).
* Documentation for the testing framework can be found [here](https://duckdb.org/dev/testing).
* Write many tests.
* Test with different types, especially numerics, strings and complex nested types.
* Try to test unexpected/incorrect usage as well, instead of only the happy path.
* `make unit` runs the **fast** unit tests (~one minute), `make allunit` runs **all** unit tests (~one hour). This will automatically (rebuild) duckdb as needed.
* To run a single test, run `make release` to build duckdb and the testrunner, and then run something like `./build/release/test/unittest test/issues/general/test_1091.test` or `./build/release/test/unittest 'test/issues/general/*'` (note the quotes to pass the `*` literally to the testrunner). Use `./build/debug/test/unittest --help` for more info.
* Make sure **all** unit tests pass before sending a PR.
* Slower tests should be added to the **all** unit tests. You can do this by naming the test file `.test_slow` in the sqllogictests, or by adding `[.]` after the test group in the C++ tests.
* Look at the code coverage report of your branch and attempt to cover all code paths in the fast unit tests. Attempt to trigger exceptions as well. It is acceptable to have some exceptions not triggered (e.g. out of memory exceptions or type switch exceptions), but large branches of code should always be either covered or removed.
* DuckDB uses GitHub Actions as its continuous integration (CI) tool. You also have the option to run GitHub Actions on your forked repository. To enable this, go to the "Actions tab" on your fork and click the enable button there to run workflows for all pushes to your fork. For more detailed instructions, you can refer to the [GitHub documentation](https://docs.github.com/en/repositories/managing-your-repositorys-settings-and-features/enabling-features-for-your-repository/managing-github-actions-settings-for-a-repository). Before running GitHub Actions, please ensure that you have all the Git tags from the duckdb/duckdb repository. To accomplish this, execute the following commands `git fetch <your-duckdb/duckdb-repo-remote-name> --tags` and then 
`git push --tags` These commands will fetch all the git tags from the duckdb/duckdb repository and push them to your forked repository. This ensures that you have all the necessary tags available for your GitHub Actions workflow. 

## Formatting

* Use tabs for indentation, spaces for alignment.
* Lines should not exceed 120 columns.
* To make sure the formatting is consistent, please use version 11.0.1, installable through `python3 -m pip install clang-format==11.0.1` or `pipx install clang-format==11.0.1`.
* `clang_format` and `black` enforce these rules automatically, use `make format-fix` to run the formatter.
* The project also comes with an [`.editorconfig` file](https://editorconfig.org/) that corresponds to these rules.

## C++ Guidelines

* Do not use `malloc`, prefer the use of smart pointers. Keywords `new` and `delete` are a code smell.
* Strongly prefer the use of `unique_ptr` over `shared_ptr`, only use `shared_ptr` if you **absolutely** have to.
* Use `const` whenever possible.
* Do **not** import namespaces (e.g. `using std`).
* All functions in source files in the core (`src` directory) should be part of the `duckdb` namespace.
* When overriding a virtual method, avoid repeating virtual and always use `override` or `final`.
* Use `[u]int(8|16|32|64)_t` instead of `int`, `long`, `uint` etc. Use `idx_t` instead of `size_t` for offsets/indices/counts of any kind.
* Prefer using references over pointers as arguments.
* Use `const` references for arguments of non-trivial objects (e.g. `std::vector`, ...).
* Use C++11 for loops when possible: `for (const auto& item : items) {...}`
* Use braces for indenting `if` statements and loops. Avoid single-line if statements and loops, especially nested ones.
* **Class Layout:** Start out with a `public` block containing the constructor and public variables, followed by a `public` block containing public methods of the class. After that follow any private functions and private variables. For example:
    ```cpp
    class MyClass {
    public:
    	MyClass();

    	int my_public_variable;

    public:
    	void MyFunction();

    private:
    	void MyPrivateFunction();

    private:
    	int my_private_variable;
    };
    ```
* Avoid [unnamed magic numbers](https://en.wikipedia.org/wiki/Magic_number_(programming)). Instead, use named variables that are stored in a `constexpr`.
* [Return early](https://medium.com/swlh/return-early-pattern-3d18a41bba8). Avoid deep nested branches.
* Do not include commented out code blocks in pull requests.
* Always use `enum class`, never use C-style `enum`. Assign an explicit storage layout (`: uint8_t`) only if the enum is intended to be serialized. 

## Error Handling

* Use exceptions **only** when an error is encountered that terminates a query (e.g. parser error, table not found). Exceptions should only be used for **exceptional** situations. For regular errors that do not break the execution flow (e.g. errors you **expect** might occur) use a return value instead.
* Try to add test cases that trigger exceptions. If an exception cannot be easily triggered using a test case then it should probably be an assertion. This is not always true (e.g. out of memory errors are exceptions, but are very hard to trigger).
* Use `D_ASSERT` to assert. Use **assert** only when failing the assert means a programmer error. Assert should never be triggered by user input. Avoid code like `D_ASSERT(a > b + 3);` without comments or context.
* Assert liberally, but make it clear with comments next to the assert what went wrong when the assert is triggered.

## Naming Conventions

* Choose descriptive names. Avoid single-letter variable names.
* Files: lowercase separated by underscores, e.g., abstract_operator.cpp
* Types (classes, structs, enums, typedefs, using): CamelCase starting with uppercase letter, e.g., BaseColumn
* Variables: lowercase separated by underscores, e.g., chunk_size
* Functions: CamelCase starting with uppercase letter, e.g., GetChunk
* Avoid `i`, `j`, etc. in **nested** loops. Prefer to use e.g. **column_idx**, **check_idx**. In a **non-nested** loop it is permissible to use **i** as iterator index.
* These rules are partially enforced by `clang-tidy`.

## Generative AI Policy

Please do not submit pull requests generated by AI (LLMs).
Reviewing such PRs puts a considerable burden on the maintainers.
