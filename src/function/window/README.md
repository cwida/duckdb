# Window Function APIs

Starting with V2.0, window functions will no longer be special cased via enums
but will be stored in the catalog as a new function type.
They are in the same namespace as macros and as scalar and aggregate functions.

Among other things, this means that extensions will be able to register _new_ window functions.
New window functions should be functions that _cannot_ be implemented as windowed aggregates.
The internal `FILL` function is an example of such a function:
it is more like `LEAD`/`LAG` than an aggregate.

Note that the builtin window functions all use this API - there are no secret internal APIs!
The one minor exception to this precept is that for backward compatibility,
the builtin functions are serialized using the legacy mechanisms.

There are five sets of APIs for window functions:

* Binding
* Blocking implementation
* Streaming implementation
* Serialization
* Registration

Note that not all APIs are required, but the streaming APIs are an "all or nothing" proposition.

## Binding APIs

The window function binding interface has two pieces:

* flags, which indicate whether functionality is supported at all;
* functions, which bind and verify the arguments.

In addition, a window function has a legacy `enum` value for serialization.
For the builtin functions, these are the old `WINDOW_RANK` enums.
Newer functions should set use the new enum `WINDOW_FUNCTION`.
This will tell the serialization code to read and write any state information that cannot be inferred.

### Binding Flags

The binding flags are used by the binder to check whether the function can support various windowing modifiers:

| Flag | Default | Description |
| :--- | :--- | :--- |
| `can_distinct` | `false` | Does the function support restricting its arguments to `DISTINCT` values? |
| `can_filter` | `false` | Does the function support filtering of its arguments? |
| `can_order_by` | `true` | Does the function support using `ORDER BY` with its arguments? |
| `can_exclude` | `false` | Does the function support frame exclusion? |
| `can_ignore_nulls` | `true` | Can the function optionally `IGNORE` or `RESPECT NULLS`? |

Most of the builtin functions use the default values of the flags.
Two notable exceptions are:

* `can_ignore_nulls` is not supported by `FILL` (interpolating `NULL`s is what it does);
* `can_order_by` is not supported by `DENSE_RANK`.

### Binding Function

The binder internals are currently only set up to validate function _arguments_,
but window functions also have ordering arguments that may need validation.
For this reason, the `window_bind_function_t` function is passed two optional arguments in the
`BindWindowFunctionInput` struct:

* `ClientContext &` - The query context;
* `WindowFunction &` - The window function being bound;
* `vector<unique_ptr<Expression>> &arguments` - The arguments being bound;
* `optional_ptr<vector<OrderByNode>> orders` - The `ORDER BY` _framing_ (not always available);
* `optional_ptr<vector<OrderByNode>> arg_orders` - The `ORDER BY` _arguments_ (not always available);

If they are present, then the binding function should validate them as well as the arguments.

Among the builtin functions, binding is only defined by the "value" functions (`XXX_VALUE`, `FILL`, `LEAD`/`LAG`).
Ordering validation is only implemented by `FILL`.
Note that the binding APIs can return an optional `FunctionData` subclass,
but none of the builtin window functions use such an object.

## Blocking APIs

The main blocking window operator (`PhysicalWindow`) has been heavily optimized
for multithreading and expression sharing.

### Framing Bounds

There are up to 8 different frame locations that may be needed by a window function,
but computing some of them can be expensive (e.g., peer boundaries).
To improve performance, window functions request which of the frame locations they need.
The set of frame locations are:

| Name | Description |
| :--- | :--- |
| `PARTITION_BEGIN` | The start of the row's _partition_ (not the hash group) |
| `PARTITION_END` | The end of the row's partition |
| `PEER_BEGIN` | The start of the row's peer group (the rows that have the same `ORDER BY` values). |
| `PEER_END` | The end of the row's peer group. |
| `VALID_BEGIN` | The start of the `ORDER BY` values that are not `NULL`. |
| `VALID_END` | The end of the `ORDER BY` values that are not `NULL`. |
| `FRAME_BEGIN` | The start of the frame (e.g., `ROWS BETWEEN`). |
| `FRAME_END` | The end of the frame. |

The `window_bounds_function_t` function fills in a set of the bounds it requires.
This may depend on details of the window expression.
Note that some of the locations require others to be computed,
but these dependencies are handled by the hosting operator.

During evaluation, these bounds are passed to the `GetData` APIs as the `bounds` argument.

### Expression Sharing

When multiple window functions are being computed for a shared partitioning and ordering,
there are often expressions that are shared between these functions.
These expressions may need to be materialized for entire partitions
(e.g., the leaves of aggregate segment trees or large lags.)
To reduce the memory footprint, the window operator requires window functions to
_register_ their expressions using the `window_sharing_function_t` API so the computed results can be shared:

* `WindowExecutor &executor` - The window function execution manager;
* `WindowSharedExpressions &sharing` - The shared expression registry.

To register an expression, the window function passes the shared expression to the `WindowSharedExpressions` object
and receives an index into the corresponding `DataChunk` or `ColumnDataCollection`.
The function then uses this index to access the expression column.
The `WindowExecutor` class provides data structures for storing these indices for use during evaluation.

There are three types of expression that can be registered:

* **Sink** expressions are passed to the `Sink` APIs as the `sink_chunk` argument.
* **Collection** expressions are passed to the `Sink` APIs as the `coll_chunk` or the `Finalize` APIs as the collection argument.
* **Evaluate** expressions are passed to the `GetData` APIs as the `eval_chunk` argument.

### State Construction

Window functions sometimes require construction of large data structures for evaluation.
The most common example in the builtin window functions are merge sort trees used for argument ordering,
but all functions share some basic information.

The window function multithreading operations use a single global (`window_global_function_t`)
and multiple thread-local (`window_local_function_t`) states per hash-group.
In the short term, these need to be derived from `WindowExecutorGlobalState` and `WindowExecutorLocalState`
respectively, although this may be refactored in the future.
There is also a potential need for an _operator_ global state per function,
but at the moment that is modelled by the `WindowExecutor` class.

After initialization, these data structures can be updated by calls to the `window_sink_function_t` API:

* `ExecutionContext &context` - The thread's execution context;
* `DataChunk &sink_chunk` - The evaluated expressions being sunk;
* `DataChunk &coll_chunk` - The evaluated expressions for the shared collection;
* `idx_t input_idx` - The row index of the chunks inside the hash group;
* `OperatorSinkInput &sink` - The sink data containing any data structures computed on the first pass.

These calls can be made on multiple threads, but the local states will as usual be confined to a single thread.

When the partition has been fully scanned and sunk to all window functions,
the `window_finalize_function_t` will be called for _each thread_:

* `ExecutionContext &context` - The thread's execution context;
* `optional_ptr<WindowCollection> collection` - A wrapper around the `ColumnDataCollection` containing all hash group sized expressions;
* `OperatorSinkInput &sink` - The sink data containing any data structures being computed.

It returns a `SourceResultType`:

* `FINISHED` - The thread is done finalizing;
* `HAVE_MORE_OUTPUT` - Call it again (with the same local state). This lets the thread yield in between the steps of a long computation,
  e.g. so that the query can be interrupted, or the progress can be reported;
* `BLOCKED` - Call it again once the task has been rescheduled through `sink.interrupt_state.Callback()`,
  e.g. when waiting for other threads to finish their part of the work.

There is currently no provision for finalizing the global state except through locks shared by the local states.

### Evaluation

With all this machinery in place, it is now possible to evaluate the function on a single chunk.
The `window_evaluate_function_t` callback is similar to scalar function evaluation,
but takes additional arguments for the frame bounds and the row index inside the hash group:

* `ExecutionContext &context` - The thread's execution context;
* `DataChunk &eval_chunk` - The function arguments;
* `DataChunk &bounds` - The 8 column bounds chunk;
* `Vector &result` - The function results;
* `idx_t row_idx` - The row index inside the hash group;
* `OperatorSinkInput &sink` - The sink data containing any data structures computed on the first pass.

## Streaming APIs

In addition to the blocking window operator, there is a streaming window operator
that can be used under certain conditions.
In order to be streamable, a window function must satisfy the following criteria:

* The `OVER()` clause must be empty (e.g., single partition, "natural ordering");
* No `ORDER BY` arguments are allowed;
* No `EXCLUDE` clause.

### Can Stream

A function that could potentially stream under these constraints can provide a `window_canstream_function_t` callback:

* `ClientContext &client` - The query's execution context;
* `const BoundWindowExpression &wexpr` - The function being checked;
* `idx_t max_delta` - The number of rows the hosting operator is willing to buffer on behalf of the function (forward or backward).

If the function says it can stream, then it _must_ provide the other two APIs: state construction and evaluation.

### Streaming State

The `window_streaming_state_function_t` API returns a subclass of `WindowExecutorStreamingState`.
It's base class is a `LocalSourceState` with an additional member that says
how many extra leading rows should be buffered.
It defaults to zero, but the subclass can override it.
The arguments are:

* `ClientContext &client` - The query context for the state;
* `DataChunk &input` - The _first_ input chunk that is being streamed;
* `const BoundWindowExpression &wexpr` - The function whose state is being constructed.

### Streaming Evaluation

The `window_stream_function_t` API evaluates the window function on a given input chunk.
The function also has access to any delayed data, but this may be larger than it has requested.
The arguments are:

* `ExecutionContext &context` - The thread-local context for the evaluation;
* `DataChunk &input` - The input chunk from the previous operator, plus the output columns being generated
* `DataChunk &delayed` - The leading rows _after_ the current chunk;
* `idx_t &delayed_capacity` - The maximum row capacity of the delayed chunk;
* `Vector &result` - The output values to generate;
* `LocalSourceState &lstate` - The thread-local state for the function.

Because this API can  be called with buffered look-ahead,
the function should treat the two chunks as consecutive blocks of rows.
In other words, the available data is `input || delayed`.
Nevertheless, the function should only compute `|input|` values for the `result`.

## Serialization APIs

The serialization APIs are used to serialize and deserialize the `FunctionData` objects created during binding.
At present, these are not needed by any of the builtin window functions,
but they are available for new functions that may require them.

## Registration

To add a new window function, you can now register it with the new window `ExtensionLoader::RegisterFunction` APIs.
You can register a single `WindowFunction`, a `WindowFunctionSet` or by using a `CreateWindowFunctionInfo` object.
The existing functions are now generated from `functions.json` files using `scripts/generate_functions.py`.
Because the script requires "subclassifications" the window functions have been split into three groups:

* `rows` - Functions that compute row numbers;
* `ranking` - Functions that compute numeric rankings;
* `value` - Functions that return argument values.

Extensions can now use these mechanisms to add new window functions to the catalog.

## Extension Example

There is an example of a simple "fill down" window function extension called "duckweed"
in `loadable_extension_demo.cpp`.
The function fills in `NULL`s using the most recent non-`NULL` value.
This operation is streamable, so it demonstrates the full API set.

(The name is the answer to "What fills up a duck?")
