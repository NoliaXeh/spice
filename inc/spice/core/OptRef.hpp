#ifndef SPICE_CORE_OPTREF_H
#define SPICE_CORE_OPTREF_H

#include <memory>
#include <type_traits>

namespace spice::core {

//! A nullable, rebindable, non-owning reference - what a raw pointer is
//! usually smuggled in for (std::optional<T&> before C++26). No arithmetic,
//! no ownership, no implicit conversion from pointers: an OptRef is either
//! bound to an object or empty, and empty is spelled `{}`.
//!
//! Dereferencing an empty OptRef is undefined, exactly like a pointer -
//! test it first: `if (auto pane { session.pane(id) }) { pane->draw(...); }`
template <typename T>
class OptRef {
public:
    OptRef() = default; //!< empty
    //! Implicit on purpose: binding a reference is the whole point.
    OptRef(T& target) : _target { std::addressof(target) } {} // NOLINT(google-explicit-constructor)
    OptRef(std::nullptr_t) = delete; //!< empty is `{}`, never nullptr

    //! OptRef<Derived> and OptRef<T> convert to OptRef<T const> etc.
    template <typename U>
        requires (std::is_convertible_v<U*, T*> && !std::is_same_v<U, T>)
    OptRef(OptRef<U> other) // NOLINT(google-explicit-constructor)
        : _target { other ? std::addressof(*other) : nullptr }
    {}

    explicit operator bool() const { return _target != nullptr; }
    auto has_value() const -> bool { return _target != nullptr; }

    auto operator*() const -> T& { return *_target; }
    auto operator->() const -> T* { return _target; }
    auto value() const -> T& { return *_target; }

    //! Two OptRefs are equal when they refer to the same object (or are
    //! both empty) - identity, not value comparison.
    friend auto operator==(OptRef lhs, OptRef rhs) -> bool {
        return lhs._target == rhs._target;
    }

private:
    T* _target { nullptr };
};

}

#endif // SPICE_CORE_OPTREF_H
