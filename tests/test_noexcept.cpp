// Compile-time noexcept checks for all noexcept-annotated hazard pointer operations.
// All assertions are evaluated at compile time; the GTest body is a marker that they passed.
#include <atomic>
#include <gtest/gtest.h>
#include <hazard_ptr.hpp>
#include <type_traits>
#include <utility>

namespace {
struct Node : proto::hazard_pointer_obj_base<Node> {};
using HP = proto::hazard_pointer;

// Construction and assignment
static_assert(std::is_nothrow_default_constructible_v<HP>);
static_assert(std::is_nothrow_move_constructible_v<HP>);
static_assert(std::is_nothrow_move_assignable_v<HP>);

// empty()
static_assert(noexcept(std::declval<const HP&>().empty()));

// protect()
static_assert(noexcept(std::declval<HP&>().protect(std::declval<std::atomic<Node*>&>())));

// try_protect()
static_assert(noexcept(std::declval<HP&>().try_protect(std::declval<Node*&>(),
                                                       std::declval<const std::atomic<Node*>&>())));

// reset_protection() -- nullptr overload
static_assert(noexcept(std::declval<HP&>().reset_protection()));
static_assert(noexcept(std::declval<HP&>().reset_protection(nullptr)));

// reset_protection(const T*) -- typed overload
static_assert(noexcept(std::declval<HP&>().reset_protection(std::declval<const Node*>())));

// swap() -- member
static_assert(noexcept(std::declval<HP&>().swap(std::declval<HP&>())));

// swap() -- free function
static_assert(noexcept(proto::swap(std::declval<HP&>(), std::declval<HP&>())));

// retire() on hazard_pointer_obj_base
static_assert(noexcept(std::declval<Node&>().retire()));
} // namespace

TEST(Noexcept, AllAnnotationsCorrect) { SUCCEED(); }
