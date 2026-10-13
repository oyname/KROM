#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>

namespace engine {

class InternedString
{
public:
    constexpr InternedString() noexcept = default;

    [[nodiscard]] const char* c_str() const noexcept
    {
        return m_data != nullptr ? m_data : "";
    }

    [[nodiscard]] std::string_view View() const noexcept
    {
        return m_data != nullptr ? std::string_view{m_data, m_size} : std::string_view{};
    }

    [[nodiscard]] bool IsValid() const noexcept
    {
        return m_data != nullptr;
    }

    explicit operator bool() const noexcept { return IsValid(); }

    [[nodiscard]] friend bool operator==(const InternedString& a, const InternedString& b) noexcept
    {
        return a.View() == b.View();
    }

    [[nodiscard]] friend bool operator!=(const InternedString& a, const InternedString& b) noexcept
    {
        return !(a == b);
    }

private:
    friend class StringPool;

    constexpr InternedString(const char* data, uint32_t size) noexcept
        : m_data(data)
        , m_size(size)
    {
    }

    const char* m_data = nullptr;
    uint32_t m_size = 0u;
};

class StringPool
{
public:
    [[nodiscard]] InternedString Intern(std::string_view text) const;

private:
    mutable std::mutex m_mutex;
    mutable std::unordered_set<std::string> m_storage;
};

} // namespace engine

namespace std {

template<>
struct hash<engine::InternedString>
{
    size_t operator()(const engine::InternedString& value) const noexcept
    {
        return std::hash<std::string_view>{}(value.View());
    }
};

} // namespace std
