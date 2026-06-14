#include "core/InternedString.hpp"

namespace engine {

InternedString StringPool::Intern(std::string_view text) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto [it, inserted] = m_storage.emplace(text);
    (void)inserted;
    return InternedString{it->c_str(), static_cast<uint32_t>(it->size())};
}

} // namespace engine
