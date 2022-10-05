#pragma once

#include <stdarg.h>
#include "esp_log.h"

namespace logger
{
    class Logger
    {
    private:
        esp_log_level_t level;

        int espLog(esp_log_level_t level, const char *tag, const char *format, va_list args);

    public:
        Logger(esp_log_level_t level);
        ~Logger();
        esp_log_level_t GetLevel() const;
        void SetLevel(esp_log_level_t level);
        void Debug(const char *tag, const char *format, ...);
        void Info(const char *tag, const char *format, ...);
        void Warn(const char *tag, const char *format, ...);
        void Error(const char *tag, const char *format, ...);
        void Level(esp_log_level_t level, const char *tag, const char *format, ...);
    };
}
