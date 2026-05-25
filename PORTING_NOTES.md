# libstdc++ porting notes

Transformations required when moving `hazard_ptr.hpp` into the libstdc++ tree. None of these are actionable in the prototype itself; this file is a checklist for the port.

- Replace `namespace proto` / `namespace proto::detail` with `namespace std` / `namespace std::__detail`.
- Replace `#pragma once` with a `_GLIBCXX_HAZARD_POINTER` include guard.
- Replace `std::hardware_destructive_interference_size` with the `__GCC_DESTRUCTIVE_SIZE` compiler built-in (same value, no `-Winterference-size` warning, drops the `<new>` include).
- Use `__glibcxx_assert` in place of `assert()`.
- Use `__gthread_mutex_t` / low-level sync primitives in place of `std::mutex` where libstdc++ convention requires it.
- Split into fine-grained libstdc++ internal includes (`<bits/move.h>` etc.) instead of public C++ standard headers.
- Annotate with `_GLIBCXX_NODISCARD`, `_GLIBCXX_NOEXCEPT`, `_GLIBCXX_BEGIN_NAMESPACE_VERSION`.
- Strip prototype comments; replace with Doxygen / libstdc++ doc style.
- Gate the `retired_` flag on the libstdc++ "contracts actually evaluated" macro rather than on `__cpp_contracts`. `__cpp_contracts` is set whenever contracts syntax is supported regardless of contract semantic (ignore / observe / enforce -- a separate compile-time switch with no standard preprocessor macro). Tie `retired_` to `_GLIBCXX_ASSERTIONS` or whatever internal macro GCC exposes for contracts-enforced builds.

## Known prototype-only workarounds

- TSan false-positive in `synchronize()` -- worked around by collect-then-snapshot ordering. Cross-atomic SC reasoning is beyond TSan's vector-clock model.
- GCC 16.1 / trunk ICE on `= default` combined with a `post()` contract on `hazard_pointer()` -- worked around with an empty `{}` body. See `hazard_pointer()` constructor in `hazard_ptr.hpp`. Tracked upstream as PR125403.
