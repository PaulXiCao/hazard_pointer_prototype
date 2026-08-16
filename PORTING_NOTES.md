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
- Replace `PROTO_NO_UNIQUE_ADDRESS` with `_GLIBCXX_NO_UNIQUE_ADDRESS` and drop the MSVC branch. The attribute is not cosmetic: without it the empty `default_delete` member costs a full aligned word and `hazard_pointer_obj_base` grows past the size the layout tripwire pins.
- Keep the `HazptrObj` base **private** and keep `hazard_pointer` a `friend` of `hazard_pointer_obj_base`. `reset_protection()` upcasts `T*` to that base, which is what makes the match key correct for `struct T : Other, hazard_pointer_obj_base<T>` ([saferecl.hp.general] p2 permits it). Folly makes its equivalent base public; private plus a friend keeps the implicit conversion out of user overload resolution.
- Carry the layout `static_assert`s over -- both of them. `hazard_pointer_obj_base` is a standard-specified type that users derive from, so its size is baked into user binaries, and `doc/xml/manual/abi.xml` lists changing the layout of a standard-specified type as a prohibited change. `hazard_pointer` is frozen for the same reason, and must stay one word: the reserved domain pointer deliberately lives in `HazptrRec`, not in the handle, so that adding custom domains later never has to widen it. The assertions are what stop a later refactor from moving either by accident.
- `HazptrRec` is created with `new` and never freed until `~HazardDomain()`. That is load-bearing, not laziness: it is what keeps the address a live `hazard_pointer` holds valid, and what lets `synchronize()` walk the record list with plain atomic loads instead of taking the allocation mutex or building a snapshot array.

The `retired_` flag is gone -- the `next == this` sentinel replaced it, so there is no longer a member whose presence depends on `__cpp_contracts`. That mattered for more than tidiness: it made `sizeof(hazard_pointer_obj_base)` differ between translation units compiled with and without contracts.

## Known prototype-only workarounds

- TSan false-positive in `synchronize()` -- worked around by collect-then-snapshot ordering. Cross-atomic SC reasoning is beyond TSan's vector-clock model.
- GCC 16.1 / trunk ICE on `= default` combined with a `post()` contract on `hazard_pointer()` -- worked around with an empty `{}` body. See `hazard_pointer()` constructor in `hazard_ptr.hpp`. Tracked upstream as PR125403.
