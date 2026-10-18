#pragma once
#include<cstddef>
#include<utility>

namespace lam {
template<std::size_t I, typename T, typename... Args>
struct type_at {
    static_assert(I < sizeof...(Args) + 1);
    using type = type_at<I - 1, Args...>::type;
};

template<typename T, typename... Args>
struct type_at<0, T, Args...> {
    using type = T;
};

template<typename... Rest> 
class Tuple;

template<typename T, typename... Rest>
class Tuple<T, Rest...> : private Tuple<Rest...> {
public : 
    using Base = Tuple<Rest...>;
    template<typename U, typename... Us>
    requires (
        sizeof...(Us) == sizeof...(Rest) && 
        !std::is_same_v<std::remove_cvref_t<U>, Tuple>
    )
    Tuple(U&& value, Us&&... rest) :
        Base(std::forward<Us>(rest)...),
        object{std::forward<U>(value)}
    {}

    static Tuple<T, Rest...> make_tuple(T value, Rest... rest) {
        Tuple<T, Rest...> ret{std::move(value), std::move(rest)...};
        return ret;
    }

    Tuple() : 
        Base(),
        object{}
    {}

    bool operator<(const Tuple<T, Rest...>& other) const {
        if (object != other.object) {
            return object < other.object;
        }
        return Base::operator<(static_cast<const Base&>(other));
    }
    
    bool operator>(const Tuple<T, Rest...>& other) const {
        if (object != other.object) {
            return object > other.object;
        }
        return Base::operator>(static_cast<const Base&>(other));
    }

    bool operator<=(const Tuple<T, Rest...>& other) const {
        return !(*this>other);
    }

    bool operator>=(const Tuple<T, Rest...>& other) const {
        return !(*this<other);
    }

    template<std::size_t I> 
    const type_at<I, T, Rest...>::type& get() const {
        if constexpr (I == 0) {
            return object;
        } else {
            return Base::template get<I - 1>();
        }
    }

    template<std::size_t I> 
    type_at<I, T, Rest...>::type& get() {
        if constexpr (I == 0) {
            return object;
        } else {
            return Base::template get<I - 1>();
        }
    }
private : 
    T object;
};

template<>
class Tuple<> {
public : 
    bool operator<(const Tuple&) const {
        return false;
    }
    bool operator>(const Tuple&) const {
        return false;
    }
    bool operator<=(const Tuple&) const {
        return false;
    }
    bool operator>=(const Tuple&) const {
        return false;
    }
};

} // namespace lam
