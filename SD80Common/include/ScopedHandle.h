#pragma once

#include <Windows.h>

namespace sd80
{
    class ScopedHandle
    {
    public:
        ScopedHandle() noexcept;

        explicit ScopedHandle(HANDLE handle) noexcept;

        ~ScopedHandle();

        ScopedHandle(const ScopedHandle&) = delete;
        ScopedHandle& operator=(const ScopedHandle&) = delete;

        ScopedHandle(ScopedHandle&& other) noexcept;
        ScopedHandle& operator=(ScopedHandle&& other) noexcept;

        HANDLE Get() const noexcept;

        bool IsValid() const noexcept;

        HANDLE Release() noexcept;

        void Reset(HANDLE handle = INVALID_HANDLE_VALUE) noexcept;

    private:
        HANDLE m_handle;
    };
}
