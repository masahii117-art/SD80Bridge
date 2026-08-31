#include "../include/ScopedHandle.h"

namespace sd80
{

    ScopedHandle::ScopedHandle() noexcept
        : m_handle(INVALID_HANDLE_VALUE)
    {
    }

    ScopedHandle::ScopedHandle(HANDLE handle) noexcept
        : m_handle(handle)
    {
    }

    ScopedHandle::~ScopedHandle()
    {
        Reset();
    }

    ScopedHandle::ScopedHandle(ScopedHandle&& other) noexcept
        : m_handle(other.Release())
    {
    }

    ScopedHandle& ScopedHandle::operator=(ScopedHandle&& other) noexcept
    {
        if (this != &other)
        {
            Reset();
            m_handle = other.Release();
        }

        return *this;
    }

    HANDLE ScopedHandle::Get() const noexcept
    {
        return m_handle;
    }

    bool ScopedHandle::IsValid() const noexcept
    {
        return
            m_handle != INVALID_HANDLE_VALUE &&
            m_handle != nullptr;
    }

    HANDLE ScopedHandle::Release() noexcept
    {
        HANDLE handle = m_handle;

        m_handle = INVALID_HANDLE_VALUE;

        return handle;
    }

    void ScopedHandle::Reset(HANDLE handle) noexcept
    {
        if (IsValid())
        {
            CloseHandle(m_handle);
        }

        m_handle = handle;
    }

}
