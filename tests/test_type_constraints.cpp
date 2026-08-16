// Compile-time tests for the HazardProtectable concept and retire() type constraints.
// All static_assert checks are evaluated at compile time; the GTest body is a marker.
#include <gtest/gtest.h>
#include <hazard_ptr.hpp>
#include <memory>
#include <string>
#include <type_traits>

namespace {

struct SimpleNode : proto::hazard_pointer_obj_base<SimpleNode> {};
struct CustomDelNode : proto::hazard_pointer_obj_base<CustomDelNode, std::default_delete<CustomDelNode>> {};

// Indirect derivation: FurtherDerived -> SimpleNode -> hazard_pointer_obj_base<SimpleNode>
// Still carries the HazptrObj base -> HazardProtectable<FurtherDerived> is true.
struct FurtherDerived : SimpleNode {};

// Not hazard-protectable.
struct PlainClass {};

} // namespace

// -- HazardProtectable concept -------------------------------------------------

// Satisfied for types that derive from any hazard_pointer_obj_base<T,D>.
static_assert(proto::detail::HazardProtectable<SimpleNode>);
static_assert(proto::detail::HazardProtectable<CustomDelNode>);
static_assert(proto::detail::HazardProtectable<FurtherDerived>);

// Not satisfied for non-class types.
static_assert(!proto::detail::HazardProtectable<int>);
static_assert(!proto::detail::HazardProtectable<void>);
static_assert(!proto::detail::HazardProtectable<int*>);

// Not satisfied for unrelated class types.
static_assert(!proto::detail::HazardProtectable<PlainClass>);
static_assert(!proto::detail::HazardProtectable<std::string>);

// -- retire() D constraints ----------------------------------------------------

// D must be invocable with T*.
static_assert(std::is_invocable_v<std::default_delete<SimpleNode>, SimpleNode*>);

// D must be default-constructible.
static_assert(std::is_default_constructible_v<std::default_delete<SimpleNode>>);

// D must be move-assignable.
static_assert(std::is_move_assignable_v<std::default_delete<SimpleNode>>);

TEST(TypeConstraints, AllConstraintsCorrect) { SUCCEED(); }
