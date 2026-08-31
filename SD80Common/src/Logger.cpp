#include "../include/Logger.h"

#include <iostream>

namespace sd80
{

    void Logger::Info(const std::wstring& message)
    {
        std::wcout << L"[INFO ] " << message << std::endl;
    }

    void Logger::Error(const std::wstring& message)
    {
        std::wcerr << L"[ERROR] " << message << std::endl;
    }

}