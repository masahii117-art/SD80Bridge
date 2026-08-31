#pragma once

#include <string>

namespace sd80
{
    class Logger
    {
    public:
        static void Info(const std::wstring& message);
        static void Error(const std::wstring& message);
    };
}