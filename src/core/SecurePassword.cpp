#include "remotefilemanager/core/SecurePassword.hpp"

#include <QChar>

#include <algorithm>
#include <limits>
#include <utility>

namespace rfm::core
{
namespace
{

void wipeMemory(char* data, std::size_t size) noexcept
{
    auto* current = reinterpret_cast<volatile unsigned char*>(data);
    while (size-- > 0) {
        *current++ = 0;
    }
}

void appendUtf8(char* output, std::size_t& offset, char32_t codePoint)
{
    if (codePoint <= 0x7fU) {
        output[offset++] = static_cast<char>(codePoint);
    } else if (codePoint <= 0x7ffU) {
        output[offset++] = static_cast<char>(0xc0U | (codePoint >> 6U));
        output[offset++] = static_cast<char>(0x80U | (codePoint & 0x3fU));
    } else if (codePoint <= 0xffffU) {
        output[offset++] = static_cast<char>(0xe0U | (codePoint >> 12U));
        output[offset++] = static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3fU));
        output[offset++] = static_cast<char>(0x80U | (codePoint & 0x3fU));
    } else {
        output[offset++] = static_cast<char>(0xf0U | (codePoint >> 18U));
        output[offset++] = static_cast<char>(0x80U | ((codePoint >> 12U) & 0x3fU));
        output[offset++] = static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3fU));
        output[offset++] = static_cast<char>(0x80U | (codePoint & 0x3fU));
    }
}

} // namespace

SecurePassword::SecurePassword(std::unique_ptr<char[]> storage, std::size_t size,
                               std::size_t capacity)
    : m_storage(std::move(storage)), m_size(size), m_capacity(capacity)
{}

SecurePassword::~SecurePassword() { clear(); }

SecurePassword::SecurePassword(SecurePassword&& other) noexcept
    : m_storage(std::move(other.m_storage)), m_size(std::exchange(other.m_size, 0)),
      m_capacity(std::exchange(other.m_capacity, 0)), m_offset(std::exchange(other.m_offset, 0))
{}

SecurePassword& SecurePassword::operator=(SecurePassword&& other) noexcept
{
    if (this != &other) {
        clear();
        m_storage = std::move(other.m_storage);
        m_size = std::exchange(other.m_size, 0);
        m_capacity = std::exchange(other.m_capacity, 0);
        m_offset = std::exchange(other.m_offset, 0);
    }
    return *this;
}

SecurePassword SecurePassword::fromUtf16(QStringView value)
{
    if (value.isEmpty()) {
        return {};
    }
    const auto characters = static_cast<std::size_t>(value.size());
    if (characters > (std::numeric_limits<std::size_t>::max() - 1U) / 3U) {
        return {};
    }
    const std::size_t capacity = characters * 3U + 1U;
    auto storage = std::make_unique<char[]>(capacity);
    std::size_t outputOffset = 0;
    for (qsizetype index = 0; index < value.size(); ++index) {
        const char16_t first = value.at(index).unicode();
        char32_t codePoint = first;
        if (QChar::isHighSurrogate(first) && index + 1 < value.size() &&
            QChar::isLowSurrogate(value.at(index + 1).unicode())) {
            codePoint = QChar::surrogateToUcs4(first, value.at(++index).unicode());
        } else if (QChar::isSurrogate(first)) {
            codePoint = QChar::ReplacementCharacter;
        }
        appendUtf8(storage.get(), outputOffset, codePoint);
    }
    storage[outputOffset] = '\0';
    return SecurePassword(std::move(storage), outputOffset, capacity);
}

bool SecurePassword::isEmpty() const { return m_storage == nullptr || m_offset >= m_size; }

const char* SecurePassword::remainingData() const
{
    return isEmpty() ? nullptr : m_storage.get() + m_offset;
}

std::size_t SecurePassword::remainingSize() const { return isEmpty() ? 0 : m_size - m_offset; }

bool SecurePassword::appendLineFeed()
{
    if (isEmpty() || m_size >= m_capacity) {
        return false;
    }
    m_storage[m_size++] = '\n';
    return true;
}

bool SecurePassword::consumeWritten(std::size_t count)
{
    if (isEmpty() || count == 0 || count > remainingSize()) {
        return false;
    }
    m_offset += count;
    if (m_offset < m_size) {
        return false;
    }
    clear();
    return true;
}

void SecurePassword::clear() noexcept
{
    if (m_storage != nullptr) {
        wipeMemory(m_storage.get(), m_capacity);
    }
    m_size = 0;
    m_offset = 0;
}

bool SecurePassword::storageIsWiped() const noexcept
{
    return m_storage == nullptr || std::all_of(m_storage.get(), m_storage.get() + m_capacity,
                                               [](char value) { return value == '\0'; });
}

} // namespace rfm::core
