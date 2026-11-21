/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include <version>

#if defined(__cpp_lib_generator)
#include <generator>
#endif
#if defined(__cpp_lib_move_only_function)
#include <functional>
#endif
#include <ranges>

#if !defined(__cpp_lib_generator) || !defined(__cpp_lib_move_only_function) || !defined(__cpp_lib_ranges_enumerate) || !defined(__cpp_lib_ranges_chunk)
#include <coroutine>
#include <exception>
#include <iterator>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#endif

namespace compat
{

#if !defined(__cpp_lib_generator)

template<typename Ref>
class generator
{
public:
    class promise_type
    {
    public:
        generator get_return_object() noexcept
        {
            return generator{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        std::suspend_always initial_suspend() noexcept
        {
            return {};
        }

        struct FinalAwaiter {
            bool await_ready() noexcept
            {
                return false;
            }
            void await_suspend(std::coroutine_handle<promise_type>) noexcept
            {
            }
            void await_resume() noexcept
            {
            }
        };
        FinalAwaiter final_suspend() noexcept
        {
            return {};
        }

        std::suspend_always yield_value(Ref value) noexcept
        {
            m_value = std::move(value);
            return {};
        }

        void return_void() noexcept
        {
        }

        void unhandled_exception() noexcept
        {
            m_exception = std::current_exception();
        }

        std::optional<Ref> m_value;
        std::exception_ptr m_exception;
    };

    generator() noexcept = default;
    explicit generator(std::coroutine_handle<promise_type> handle) noexcept
        : m_handle(handle)
    {
    }
    generator(generator &&other) noexcept
        : m_handle(std::exchange(other.m_handle, {}))
    {
    }
    generator(const generator &) = delete;
    generator &operator=(const generator &) = delete;
    generator &operator=(generator &&other) noexcept
    {
        if (this != &other) {
            if (m_handle) {
                m_handle.destroy();
            }
            m_handle = std::exchange(other.m_handle, {});
        }
        return *this;
    }
    ~generator()
    {
        if (m_handle) {
            m_handle.destroy();
        }
    }

    bool done() const noexcept
    {
        return !m_handle || m_handle.done();
    }

    class iterator
    {
    public:
        using value_type = std::remove_cvref_t<Ref>;
        using reference = Ref &;
        using difference_type = std::ptrdiff_t;
        using iterator_concept = std::input_iterator_tag;

        explicit iterator(generator *gen) noexcept
            : m_gen(gen)
        {
        }

        reference operator*() const
        {
            return *m_gen->m_handle.promise().m_value;
        }

        iterator &operator++()
        {
            m_gen->resume();
            return *this;
        }
        void operator++(int)
        {
            ++*this;
        }

        bool operator==(std::default_sentinel_t) const
        {
            return m_gen->done();
        }

    private:
        generator *m_gen;
    };

    iterator begin()
    {
        resume();
        return iterator{this};
    }

    std::default_sentinel_t end() noexcept
    {
        return {};
    }

private:
    void resume()
    {
        if (done()) {
            return;
        }
        m_handle.resume();
        if (m_handle.promise().m_exception) {
            std::rethrow_exception(m_handle.promise().m_exception);
        }
    }

    std::coroutine_handle<promise_type> m_handle;
};

#else

using std::generator;

#endif

#if !defined(__cpp_lib_move_only_function)

template<typename Signature>
class move_only_function;

template<typename R, typename... Args>
class move_only_function<R(Args...)>
{
    struct Base {
        virtual ~Base() = default;
        virtual R invoke(Args... args) = 0;
    };

    template<typename F>
    struct Impl final : Base {
        std::decay_t<F> m_fn;

        explicit Impl(F &&f)
            : m_fn(std::forward<F>(f))
        {
        }

        R invoke(Args... args) override
        {
            return m_fn(std::forward<Args>(args)...);
        }
    };

public:
    move_only_function() noexcept = default;
    move_only_function(std::nullptr_t) noexcept
    {
    }
    move_only_function(move_only_function &&) noexcept = default;
    move_only_function &operator=(move_only_function &&) noexcept = default;
    move_only_function(const move_only_function &) = delete;
    move_only_function &operator=(const move_only_function &) = delete;

    template<typename F, typename = std::enable_if_t<!std::is_same_v<std::decay_t<F>, move_only_function>>>
    move_only_function(F &&f)
        : m_impl(std::make_unique<Impl<F>>(std::forward<F>(f)))
    {
    }

    R operator()(Args... args)
    {
        return m_impl->invoke(std::forward<Args>(args)...);
    }

    explicit operator bool() const noexcept
    {
        return m_impl != nullptr;
    }

private:
    std::unique_ptr<Base> m_impl;
};

#else

using std::move_only_function;

#endif

}

namespace compat::views
{

#if defined(__cpp_lib_ranges_enumerate)

template<typename Range>
auto enumerate(Range &range)
{
    return std::views::enumerate(range);
}

#else

template<typename Range>
class enumerate_view
{
public:
    explicit enumerate_view(Range &range)
        : m_range(range)
    {
    }

    auto begin() const
    {
        using std::begin;
        return iterator{begin(m_range), 0};
    }

    auto end() const
    {
        using std::end;
        return end(m_range);
    }

private:
    using underlying_iterator = std::conditional_t<std::is_const_v<Range>,
                                                   typename std::add_const_t<std::remove_reference_t<Range>>::iterator,
                                                   typename std::remove_reference_t<Range>::iterator>;

    struct iterator {
        underlying_iterator it;
        std::size_t index;

        std::pair<std::size_t, decltype(*std::declval<underlying_iterator>())> operator*() const
        {
            return {index, *it};
        }

        iterator &operator++()
        {
            ++it;
            ++index;
            return *this;
        }
        void operator++(int)
        {
            ++*this;
        }

        bool operator==(underlying_iterator sentinel) const
        {
            return it == sentinel;
        }
    };

    Range &m_range;
};

template<typename Range>
auto enumerate(Range &range)
{
    return enumerate_view<Range>{range};
}

#endif

#if defined(__cpp_lib_ranges_chunk)

using std::views::chunk;

#else

template<std::ranges::forward_range V>
    requires std::ranges::view<V>
class chunk_view : public std::ranges::view_interface<chunk_view<V>>
{
    using base_iter = std::ranges::iterator_t<V>;
    using base_sentinel = std::ranges::sentinel_t<V>;

public:
    chunk_view() = default;
    chunk_view(V base, std::size_t chunkSize)
        : m_base(std::move(base))
        , m_chunkSize(chunkSize)
    {
    }

    class iterator
    {
    public:
        using difference_type = std::ptrdiff_t;
        using value_type = std::ranges::subrange<base_iter>;
        using iterator_concept = std::forward_iterator_tag;

        iterator() = default;
        iterator(base_iter cur, base_sentinel end, std::size_t chunkSize)
            : m_cur(cur)
            , m_end(end)
            , m_chunkSize(chunkSize)
        {
        }

        value_type operator*() const
        {
            auto next = m_cur;
            std::ranges::advance(next, static_cast<std::ranges::range_difference_t<V>>(m_chunkSize), m_end);
            return std::ranges::subrange(m_cur, next);
        }

        iterator &operator++()
        {
            std::ranges::advance(m_cur, static_cast<std::ranges::range_difference_t<V>>(m_chunkSize), m_end);
            return *this;
        }

        iterator operator++(int)
        {
            auto tmp = *this;
            ++*this;
            return tmp;
        }

        bool operator==(const iterator &other) const
        {
            return m_cur == other.m_cur;
        }

        bool operator==(std::default_sentinel_t) const
        {
            return m_cur == m_end;
        }

    private:
        base_iter m_cur{};
        base_sentinel m_end{};
        std::size_t m_chunkSize = 0;
    };

    iterator begin()
    {
        return iterator{std::ranges::begin(m_base), std::ranges::end(m_base), m_chunkSize};
    }

    std::default_sentinel_t end() const noexcept
    {
        return {};
    }

    std::size_t size() const
        requires std::ranges::sized_range<const V>
    {
        const auto total = static_cast<std::size_t>(std::ranges::size(m_base));
        return total / m_chunkSize + (total % m_chunkSize != 0 ? 1 : 0);
    }

    std::size_t size()
        requires std::ranges::sized_range<V> && (!std::ranges::sized_range<const V>)
    {
        const auto total = static_cast<std::size_t>(std::ranges::size(m_base));
        return total / m_chunkSize + (total % m_chunkSize != 0 ? 1 : 0);
    }

private:
    V m_base{};
    std::size_t m_chunkSize = 0;
};

struct chunk_closure {
    std::size_t chunkSize;

    template<std::ranges::viewable_range Range>
    friend auto operator|(Range &&range, chunk_closure closure)
    {
        return chunk_view{std::views::all(std::forward<Range>(range)), closure.chunkSize};
    }
};

struct chunk_fn {
    template<std::ranges::viewable_range Range>
    auto operator()(Range &&range, std::size_t chunkSize) const
    {
        return chunk_view{std::views::all(std::forward<Range>(range)), chunkSize};
    }

    constexpr chunk_closure operator()(std::size_t chunkSize) const noexcept
    {
        return chunk_closure{chunkSize};
    }
};

inline constexpr chunk_fn chunk{};

#endif
}
