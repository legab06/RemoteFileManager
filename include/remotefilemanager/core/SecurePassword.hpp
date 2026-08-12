#pragma once

#include <QStringView>

#include <cstddef>
#include <memory>

namespace rfm::core
{

class SecurePassword final
{
  public:
    SecurePassword() = default;
    ~SecurePassword();

    SecurePassword(const SecurePassword&) = delete;
    SecurePassword& operator=(const SecurePassword&) = delete;
    SecurePassword(SecurePassword&& other) noexcept;
    SecurePassword& operator=(SecurePassword&& other) noexcept;

    [[nodiscard]] static SecurePassword fromUtf16(QStringView value);
    [[nodiscard]] bool isEmpty() const;
    [[nodiscard]] const char* remainingData() const;
    [[nodiscard]] std::size_t remainingSize() const;

    // Reserves one byte during UTF-8 encoding, so this never reallocates a UI password.
    [[nodiscard]] bool appendLineFeed();
    // Returns true and wipes the complete controlled allocation after the final byte.
    [[nodiscard]] bool consumeWritten(std::size_t count);
    void clear() noexcept;
    [[nodiscard]] bool storageIsWiped() const noexcept;

  private:
    SecurePassword(std::unique_ptr<char[]> storage, std::size_t size, std::size_t capacity);

    std::unique_ptr<char[]> m_storage;
    std::size_t m_size{0};
    std::size_t m_capacity{0};
    std::size_t m_offset{0};
};

} // namespace rfm::core
