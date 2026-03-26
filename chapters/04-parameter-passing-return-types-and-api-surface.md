# Parameter Passing, Return Types, and API Surface

In C++, a function signature says more than many authors intend. It says whether the callee borrows or retains data. It often implies whether null is meaningful, whether mutation is allowed, whether a copy may happen, and whether failure is ordinary control flow. If the signature gets these semantics wrong, the implementation can still be locally correct while the API remains expensive to use and hard to review.

This chapter is about that semantic surface area. The goal is not to memorize "always pass X by Y." The goal is to choose parameter and return forms that tell the truth about ownership, lifetime, mutation, nullability, and cost at the boundary where a caller must make decisions.

That boundary focus keeps this chapter distinct from the ones before and after it. Chapter 1 dealt with who owns resources. Chapter 2 dealt with what kinds of objects exist in the model. Chapter 3 dealt with how failure should cross boundaries. Here the question is narrower and more practical: given those design choices, what should a function signature look like so the call contract is obvious?

## Signatures Are Contracts, Not Type-Checking Rituals

Many bad C++ APIs come from treating the signature as the smallest set of types that compiles. That approach ignores the fact that parameter and return choices are documentation with teeth.

Take a parser boundary.

```cpp
auto parse_frame(std::span<const std::byte> bytes)
	-> std::expected<Frame, ParseError>;
```

This single line communicates several things immediately.

- The function borrows contiguous read-only bytes.
- It does not require ownership of the source buffer.
- A frame is produced as an owned value.
- Failure is expected and explicit.

Compare that with `Frame parse_frame(const std::vector<std::byte>&);`. That version implies a container choice the parser does not need, hides failure policy, and says nothing about whether the returned `Frame` contains borrowed views into the input or independent owned data.

The example project's HTTP parser follows the same pattern. In `examples/web-api/src/modules/http.cppm`, `parse_request` borrows its input and returns an owned result:

```cpp
[[nodiscard]] inline std::optional<Request>
parse_request(std::string_view raw);
```

The function accepts a `string_view` into a stack buffer, parses method, path, headers, and body, and returns a fully owned `Request` with `std::string` members. The caller's buffer can be reused or destroyed immediately after the call returns. That borrow-in, own-out contract is visible in the signature alone.

The difference is not stylistic polish. It is whether the call site can reason about the contract without opening the implementation.

## Borrowing Parameters Should Look Borrowed

If a function reads caller-owned data during the call and does not retain it, the signature should express borrowing directly.

For text, `std::string_view` is usually the right parameter type when null termination is irrelevant and no ownership transfer occurs. For contiguous binary or element sequences, `std::span<const T>` is usually the right read-only form. For mutable borrowed access, `std::span<T>` or a non-const reference may be appropriate depending on whether the abstraction is sequence-shaped or object-shaped.

This has two advantages.

1. Call sites remain flexible. They can pass strings, slices, arrays, vectors, and mapped buffers without forced allocation or container conversion.
2. The contract is honest. Borrowing stays borrowing.

The main misuse is allowing borrowed parameters to leak into retained state. A function that accepts `string_view` and then caches it beyond the call is not clever; it is lying about the contract.

### Dangling Borrows: The Cost of Getting This Wrong

When a borrowed parameter outlives its source, the result is undefined behavior that often manifests as intermittent corruption rather than a clean crash:

```cpp
class Logger {
public:
	void set_prefix(std::string_view prefix) {
		prefix_ = prefix; // BUG: stores a view, not a copy
	}

	void log(std::string_view message) {
		fmt::print("[{}] {}\n", prefix_, message); // reads dangling view
	}

private:
	std::string_view prefix_; // non-owning -- lifetime depends on caller
};

void configure_logger(Logger& logger) {
	std::string name = build_service_name();
	logger.set_prefix(name); // name is destroyed at end of scope
} // name destroyed here -- logger.prefix_ is now dangling
```

The fix is straightforward: if the member must outlive the call, it must own its data.

```cpp
class Logger {
public:
	void set_prefix(std::string prefix) { // takes ownership by value
		prefix_ = std::move(prefix);
	}
	// ...
private:
	std::string prefix_; // owning -- no lifetime dependency on caller
};
```

That is why a useful review heuristic is simple: if the parameter type says borrow, all retention must be visible in the implementation as an explicit copy or transformation to an owning type.

## Pass by Value When the Callee Needs Its Own Copy Anyway

One of the most useful modern C++ patterns is passing by value when the callee intends to store or otherwise own the argument. This often surprises people trained to avoid copies at all costs.

Consider a request object that stores a tenant name.

```cpp
class RequestContext {
public:
	explicit RequestContext(std::string tenant)
		: tenant_(std::move(tenant)) {}

private:
	std::string tenant_;
};
```

This constructor is often better than both `const std::string&` and `std::string_view`.

- It is honest that the object will own a string.
- Lvalue callers pay one copy, which was unavoidable anyway.
- Rvalue callers can move directly.
- There is no temptation to retain a borrowed view accidentally.

The rule is not "always pass expensive types by const reference." The rule is "pass by value when ownership transfer into the callee is the intended contract and the extra move/copy story is acceptable."

### Wrong Parameter Choices and Their Costs

The cost of getting parameter passing wrong is not always dramatic, but it compounds across hot paths and large objects.

**Unnecessary copy from `const std::string&` when ownership is needed:**

```cpp
class Registry {
public:
	void register_name(const std::string& name) {
		names_.push_back(name); // always copies, even if caller passed a temporary
	}
private:
	std::vector<std::string> names_;
};

// Caller:
registry.register_name(build_name()); // builds a temporary string, copies it,
									  // then destroys the temporary. The move
									  // that pass-by-value would have enabled
									  // is lost.
```

With pass-by-value-and-move, the temporary is moved directly into the container at zero copy cost:

```cpp
void register_name(std::string name) {
	names_.push_back(std::move(name)); // rvalue callers: 1 move. lvalue callers: 1 copy + 1 move.
}
```

The example project's error module applies the same pattern. In `examples/web-api/src/modules/error.cppm`, `make_error` takes a `std::string` by value and moves it into the error object:

```cpp
[[nodiscard]] inline std::unexpected<Error>
make_error(ErrorCode code, std::string detail) {
    return std::unexpected<Error>{Error{code, std::move(detail)}};
}
```

Callers passing a string literal or a temporary pay zero copy cost; callers passing an lvalue pay one copy that the function would have needed anyway. The signature honestly communicates that `detail` will be owned by the resulting error.

**Forced allocation from `const std::vector<T>&` when `std::span` suffices:**

```cpp
// Anti-pattern: forces callers to allocate a vector even if data is in an array or span.
double average(const std::vector<double>& values);

// Caller with a C array or std::array must construct a vector just to call this:
std::array<double, 4> readings = {1.0, 2.0, 3.0, 4.0};
auto avg = average(std::vector<double>(readings.begin(), readings.end())); // pointless heap allocation
```

With `std::span<const double>`, the function accepts any contiguous source without forcing a container choice:

```cpp
double average(std::span<const double> values);

// Now works with vector, array, C array, span -- no allocation required.
auto avg = average(readings);
```

The tradeoff is that pass-by-value can be wrong for polymorphic types, very large aggregates where copying lvalues is rarely desired, or APIs where retention is conditional and uncommon. As always, the semantic contract comes first.

## Non-const Reference Means More Than Mutability

A non-const reference parameter is strong syntax. It says the caller must provide a live object, null is not meaningful, and the callee may mutate that exact object. This is sometimes the right contract. It is also overused.

Use a non-const reference when the mutation is central to the operation and callers should see it as the main point of the call. Sorting a vector in place, filling a provided output buffer, or advancing a parser state object may fit.

Do not use non-const references merely to avoid returning a value or because out-parameters feel familiar from C APIs. Out-parameters weaken readability when the result is conceptually the output of the function rather than an object the caller is deliberately handing over for mutation.

In modern C++, returning a value is usually clearer for primary results. Reserve non-const reference parameters for genuine in-place mutation or multi-object coordination where mutation is the real contract.

## Raw Pointers Are Mostly for Optionality and Interop

Raw pointers still have legitimate roles in interfaces. The cleanest modern use is to represent an optional borrowed object or to interoperate with lower-level APIs.

That is a narrower role than many codebases give them.

A `T*` parameter should usually mean one of two things:

1. The callee may receive no object at all.
2. The interface is crossing into pointer-based interop or low-level data structures where pointer identity itself matters.

If null is not meaningful, a reference is usually clearer. If ownership is being transferred, `std::unique_ptr<T>` or another owning type is clearer. If the object is an array or contiguous sequence, `std::span<T>` is usually clearer. A naked pointer that means "non-null borrowed maybe-single maybe-many maybe-retained" is semantic debt.

The same principle applies to return types. Returning a raw owning pointer from ordinary modern C++ APIs is almost always the wrong signal. Returning a raw observer pointer can be fine when absence is meaningful and lifetime is controlled elsewhere.

## Return Owned Meaning, Not Storage Accidents

Return types deserve the same discipline as parameters. The main question is whether the caller should receive owned meaning, borrowed access, or a decision-bearing wrapper such as `expected` or `optional`.

For many APIs, returning an owned value is the cleanest design even when it involves a move. This keeps lifetime local, makes composition easier, and avoids callers depending on internal storage. Modern C++23 move semantics make value returns cheap enough in many cases that the clarity win dominates.

Borrowed return types are appropriate only when the source lifetime is obvious, stable, and truly part of the contract. Returning `std::string_view` into internal storage is fine only when the storage clearly outlives the view and callers can use that fact safely. Across broad boundaries, this is often a poor trade because it exports lifetime reasoning the callee could have kept private.

Optionality and failure should also be explicit in the return type rather than smuggled through sentinel values. A search returning "maybe found" fits `std::optional<T>` or an observer pointer if lifetime semantics require it. A parse or load operation whose failure is decision-relevant fits `std::expected<T, E>`. A function that returns an empty string or `-1` on failure is usually making the API weaker than the implementation needs it to be.

## Anti-pattern: One Signature, Several Hidden Stories

This kind of API survives in many codebases because it seems flexible.

```cpp
// Anti-pattern: signature hides ownership, failure, and buffer contract.
bool encode_record(const Record& record,
			   std::vector<std::byte>& output,
			   std::string* error_message = nullptr);
```

This one function now carries several hidden rules.

- Does it append to `output` or overwrite it?
- Is `error_message` optional because diagnostics are not important, or because logging happens elsewhere?
- Can `output` be partially modified on failure?
- Is `false` validation failure, encoding bug, capacity issue, or internal exception translation?

Nothing in the signature answers those questions cleanly.

A stronger API usually splits the semantics.

```cpp
auto encode_record(const Record& record)
	-> std::expected<std::vector<std::byte>, EncodeError>;

auto append_encoded_record(const Record& record,
				   ByteAppender& output)
	-> std::expected<void, EncodeError>;
```

Now the caller chooses between owned result production and append-style mutation, and the failure contract is explicit. The two different operations no longer pretend to be one generic "flexible" interface.

## Factories and Acquisition Functions Must State Ownership Up Front

Creation functions are where unclear ownership becomes especially expensive. A factory returning `T*` leaves callers asking who deletes it. A factory writing into an out-parameter plus bool return often hides partial-construction rules. A factory returning `shared_ptr<T>` by default may impose shared ownership long before the design proved it necessary.

For ordinary exclusive ownership, `std::unique_ptr<T>` is usually the clearest result. For value-like created objects, return the value directly or use `expected<T, E>` when failure belongs at the boundary. For shared ownership, return `shared_ptr<T>` only when the product being created is genuinely intended for shared lifetime.

The difference is concrete:

```cpp
// Anti-pattern: raw pointer factory -- caller does not know who owns the result.
Widget* create_widget(const WidgetConfig& cfg);

void setup() {
	auto* w = create_widget(cfg);
	// Does the caller own w? Does a global registry own it?
	// Must the caller call delete? delete[]? A custom deallocator?
	// Nothing in the signature answers these questions.
	use(w);
	// If the caller guesses wrong, the result is a leak or a double-free.
}
```

```cpp
// Clear: unique_ptr states exclusive caller ownership unambiguously.
auto create_widget(const WidgetConfig& cfg)
	-> std::expected<std::unique_ptr<Widget>, WidgetError>;

void setup() {
	auto result = create_widget(cfg);
	if (!result) { /* handle error */ }
	auto widget = std::move(*result); // ownership transferred, no ambiguity
	// widget is destroyed automatically when it leaves scope
}
```

The example project shows a value-oriented variant of this pattern. In `examples/web-api/src/modules/task.cppm`, `Task::validate` is a factory-style function that takes a `Task` by value and returns `Result<Task>` (an alias for `std::expected<Task, Error>`):

```cpp
[[nodiscard]] static Result<Task> validate(Task t) {
    if (t.title.empty()) {
        return make_error(ErrorCode::bad_request, "title must not be empty");
    }
    return t;
}
```

And in `examples/web-api/src/modules/repository.cppm`, `TaskRepository::create` composes with it — accepting a `Task` by value, validating, assigning an ID, and returning the stored result or the validation error:

```cpp
[[nodiscard]] Result<Task> create(Task task);
```

Neither function uses out-parameters or bool return codes. The ownership story is the same as the `unique_ptr` factory above, adapted for value types: the caller moves a value in, gets back either a valid owned result or an explicit error.

The important point is not the specific vocabulary type. It is that creation boundaries are where ownership should become unmistakable.

## API Surface Is Also Cost Surface

Signature choices influence cost in ways that matter to callers.

A `std::function` parameter may allocate and type-erase even when the callback is used only synchronously. A `std::span<const T>` avoids forcing callers into a container representation. A `std::string` by-value sink constructor may permit efficient moves from temporaries. Returning an owned vector may allocate once but eliminate a long-lived lifetime hazard. These are design tradeoffs, not micro-optimizations.

The right discipline is to expose costs the caller should know about and avoid accidental ones the caller cannot infer. A good signature does not promise zero cost. It makes the important costs unsurprising.

That is also why broad "convenience" overload sets can become harmful. When an API accepts every combination of pointer, string, span, vector, and view, the overload surface can become harder to reason about than the original problem. Prefer a small number of semantically crisp forms.

## Verification and Review

Function signatures are one of the cheapest places to catch design mistakes early.

Useful review questions:

1. Does each parameter truthfully communicate borrow, ownership transfer, mutation, or optionality?
2. Is pass-by-value being used where the callee needs ownership, rather than out of habit or dogma?
3. Are raw pointers reserved for optional borrowed access or interop, rather than vague contracts?
4. Does the return type express owned result, borrowed access, or explicit failure clearly?
5. Is the API exposing the important costs and hiding only incidental implementation details?

Tests should exercise signature-driven semantics, not only core behavior. Verify append versus overwrite behavior. Verify that returned views remain valid for the documented lifetime and no longer. Verify that failure leaves output parameters or state in the promised condition. A clear signature still needs evidence behind it.

## Takeaways

- Treat signatures as semantic contracts, not just compiler-acceptable types.
- Use borrowing parameter types when the callee only inspects caller-owned data.
- Pass by value when the callee needs to take ownership and that contract should be obvious.
- Use references, pointers, and return wrappers to express mutation, optionality, and failure deliberately.
- Keep the API surface small enough that callers can understand lifetime and cost without reading implementation code.

Good C++ APIs do not merely compile. They tell the truth early enough that callers can use them correctly on the first read. That is what makes signatures worth this much attention.