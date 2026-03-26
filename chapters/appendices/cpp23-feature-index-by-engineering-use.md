# Appendix A: C++23 Feature Index by Engineering Use

This is not a changelog of everything added in C++23. It is the short list of language and library facilities that materially change design in a C++23 codebase. Use it to locate the right tool for an engineering pressure, then read the referenced chapters for the tradeoffs.

## Make Ownership and Retention Obvious

| Engineering pressure | Primary tools | Use them when | Main risk | See |
|---|---|---|---|---|
| Exclusive ownership of a resource | `std::unique_ptr`, direct members, move-only RAII wrappers | One component clearly owns cleanup and transfer should be explicit | Hiding ownership behind raw pointers or shared ownership by convenience | Chapters 1, 4 |
| Borrowing without transfer | `std::string_view`, `std::span` | The callee inspects caller-owned contiguous data and does not retain it | Storing the borrow or carrying it across async boundaries | Chapters 4, 5 |
| Shared lifetime across tasks or subsystems | `std::shared_ptr`, `std::weak_ptr` | There is a real multi-owner lifetime that cannot be expressed more simply | Lost destruction timing, refcount traffic, and vague ownership | Chapters 1, 12, 13 |

## Represent Absence, Failure, and Alternatives Precisely

| Engineering pressure | Primary tools | Use them when | Main risk | See |
|---|---|---|---|---|
| Ordinary absence | `std::optional` | Missing data is expected and not itself an error | Erasing why work failed when callers need diagnostics | Chapters 3, 5 |
| Recoverable failure at a boundary | `std::expected` | The caller must make a visible decision based on failure | Propagating `expected` so deep that local code becomes ceremony | Chapters 3, 5, 21, 22 |
| Closed set of valid alternatives | `std::variant` | Several states are all legitimate and must be handled explicitly | Using it where the state space is open-ended or unstable | Chapters 5, 10 |

## Constrain Generic Code Without Lying About It

| Engineering pressure | Primary tools | Use them when | Main risk | See |
|---|---|---|---|---|
| Reject nonsense template use early | concepts, `requires`, named constraints | The template surface is public or heavily reused | Constraint sets that are harder to understand than the code they protect | Chapters 6, 22 |
| Adapt to ranges instead of container types | `std::ranges`, views, range algorithms | The algorithm works on a sequence abstraction rather than one owning container | Composing pipelines whose lifetime or traversal cost is no longer obvious | Chapters 7, 15 |

## Express Work Over Time Deliberately

| Engineering pressure | Primary tools | Use them when | Main risk | See |
|---|---|---|---|---|
| Cooperative stop and thread ownership | `std::jthread`, `std::stop_source`, `std::stop_token` | Work must stop cleanly during shutdown, deadline expiry, or parent failure | Wiring tokens through code without defining what cancellation means | Chapters 14, 21 |
| Suspended async work with local clarity | coroutines, coroutine-based task types | Async control flow is complex enough that callback state machines are obscuring ownership and failure | Borrowed state outliving the caller or tasks with no clear owner | Chapters 13, 14 |
| Incremental pull-style production | generators | The consumer should pull a sequence lazily without materializing it all at once | Yielding references into storage whose lifetime is unstable | Chapter 7 |

## Control Allocation and Data Movement

| Engineering pressure | Primary tools | Use them when | Main risk | See |
|---|---|---|---|---|
| Caller-controlled allocation strategy | `std::pmr` facilities | Allocation policy materially affects performance or integration | Exposing allocator policy where it is not part of the contract | Chapters 16, 22 |
| Stable contiguous ownership for hot paths | `std::vector`, `std::array`, flat contiguous representations | Locality and predictable traversal matter more than node stability | Overfitting container choice to one benchmark or one workload | Chapters 15, 16, 17 |

## Move Work to Compile Time Carefully

| Engineering pressure | Primary tools | Use them when | Main risk | See |
|---|---|---|---|---|
| Remove runtime branching or validation cost | `constexpr`, `consteval`, compile-time tables | The payoff is real and the generated surface stays readable | Turning build times and diagnostics into the new bottleneck | Chapter 8 |
| Make contracts visible to the compiler | type traits, constrained templates, structural compile-time checks | Incorrect instantiations or illegal states should fail early | Producing metaprogramming that only its author can review | Chapters 6, 8 |

## Keep the Index Honest

- Prefer the tool that makes the contract easiest to review, not the tool with the newest spelling.
- If a feature removes one local cost by creating hidden lifetime, shutdown, ABI, or measurement risk elsewhere, the feature is not helping.
- When in doubt, choose the representation that makes ownership, failure, and cost visible in the type system or at the boundary.