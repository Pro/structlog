// Copyright(c) 2015-present, Gabi Melman & spdlog contributors.
// Distributed under the MIT License (http://opensource.org/licenses/MIT)

#pragma once

// Thread safe logger (except for set_error_handler())
// Has name, log level, vector of std::shared sink pointers and formatter
// Upon each log write the logger:
// 1. Checks if its log level is enough to log the message and if yes:
// 2. Call the underlying sinks to do the job.
// 3. Each sink use its own private copy of a formatter to format the message
// and send to its destination.
//
// The use of private formatter per sink provides the opportunity to cache some
// formatted data, and support for different format per sink.

#include <spdlog/common.h>
#include <spdlog/details/log_msg.h>
#include <spdlog/details/backtracer.h>
#ifdef SPDLOG_HEADER_ONLY
#    undef SPDLOG_HEADER_ONLY
#    include <spdlog/details/executor.h>
#    define SPDLOG_HEADER_ONLY
#else
#    include <spdlog/details/executor.h>
#endif
#include <spdlog/json_formatter.h>

#ifdef SPDLOG_WCHAR_TO_UTF8_SUPPORT
#    ifndef _WIN32
#        error SPDLOG_WCHAR_TO_UTF8_SUPPORT only supported on windows
#    endif
#    include <spdlog/details/os.h>
#endif

#include <vector>

#ifndef SPDLOG_NO_EXCEPTIONS
#    define SPDLOG_LOGGER_CATCH(location)                                                                                                  \
        catch (const std::exception &ex)                                                                                                   \
        {                                                                                                                                  \
            if (location.filename)                                                                                                         \
            {                                                                                                                              \
                err_handler_(fmt_lib::format(SPDLOG_FMT_STRING("{} [{}({})]"), ex.what(), location.filename, location.line));              \
            }                                                                                                                              \
            else                                                                                                                           \
            {                                                                                                                              \
                err_handler_(ex.what());                                                                                                   \
            }                                                                                                                              \
        }                                                                                                                                  \
        catch (...)                                                                                                                        \
        {                                                                                                                                  \
            err_handler_("Rethrowing unknown exception in logger");                                                                        \
            throw;                                                                                                                         \
        }
#else
#    define SPDLOG_LOGGER_CATCH(location)
#endif

namespace spdlog {

namespace details {
    class registry;
}

class SPDLOG_API logger
{
public:
    // Empty logger
    explicit logger(std::string name)
        : name_(std::move(name))
        , sinks_()
    {}

    // Logger with range on sinks
    template<typename It>
    logger(std::string name, It begin, It end)
        : name_(std::move(name))
        , sinks_(begin, end)
    {}

    // Logger with single sink
    logger(std::string name, sink_ptr single_sink)
        : logger(std::move(name), {std::move(single_sink)})
    {}

    // Logger with sinks init list
    logger(std::string name, sinks_init_list sinks)
        : logger(std::move(name), sinks.begin(), sinks.end())
    {}

    virtual ~logger() = default;

    logger(const logger &other);
    logger(logger &&other) SPDLOG_NOEXCEPT;
    logger &operator=(logger other) SPDLOG_NOEXCEPT;
    void swap(spdlog::logger &other) SPDLOG_NOEXCEPT;

    template<typename... Args>
    SPDLOG_EXECUTOR_T log(source_loc loc, level::level_enum lvl, format_string_t<Args...> fmt, Args &&... args)
    {
        return log_(loc, lvl, fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    SPDLOG_EXECUTOR_T log(level::level_enum lvl, format_string_t<Args...> fmt, Args &&... args)
    {
        return log(source_loc{}, lvl, fmt, std::forward<Args>(args)...);
    }

    template<typename T>
    SPDLOG_EXECUTOR_T log(level::level_enum lvl, const T &msg)
    {
        return log(source_loc{}, lvl, msg);
    }

    // T cannot be statically converted to format string (including string_view/wstring_view)
    template<class T, typename std::enable_if<!is_convertible_to_any_format_string<const T &>::value, int>::type = 0>
    SPDLOG_EXECUTOR_T log(source_loc loc, level::level_enum lvl, const T &msg)
    {
        return log(loc, lvl, "{}", msg);
    }

    SPDLOG_EXECUTOR_T log(log_clock::time_point log_time, source_loc loc, level::level_enum lvl, string_view_t msg)
    {
        bool log_enabled = should_log(lvl);
        bool traceback_enabled = tracer_.enabled();
        if (!log_enabled && !traceback_enabled)
        {
            return SPDLOG_EXECUTOR_T{};
        }

        details::log_msg log_msg(log_time, loc, name_, lvl, msg);
        return log_it_(log_msg, log_enabled, traceback_enabled);
    }

    SPDLOG_EXECUTOR_T log(source_loc loc, level::level_enum lvl, string_view_t msg)
    {
        bool log_enabled = should_log(lvl);
        bool traceback_enabled = tracer_.enabled();
        if (!propagate_ && !log_enabled && !traceback_enabled)
        {
            return SPDLOG_EXECUTOR_T{};
        }

        details::log_msg log_msg(loc, name_, lvl, msg);
        return log_it_(log_msg, log_enabled, traceback_enabled);
    }

    SPDLOG_EXECUTOR_T log(level::level_enum lvl, string_view_t msg)
    {
        return log(source_loc{}, lvl, msg);
    }

    template<typename... Args>
    SPDLOG_EXECUTOR_T trace(format_string_t<Args...> fmt, Args &&... args)
    {
        return log(level::trace, fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    SPDLOG_EXECUTOR_T debug(format_string_t<Args...> fmt, Args &&... args)
    {
        return log(level::debug, fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    SPDLOG_EXECUTOR_T info(format_string_t<Args...> fmt, Args &&... args)
    {
        return log(level::info, fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    SPDLOG_EXECUTOR_T warn(format_string_t<Args...> fmt, Args &&... args)
    {
        return log(level::warn, fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    SPDLOG_EXECUTOR_T error(format_string_t<Args...> fmt, Args &&... args)
    {
        return log(level::err, fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    SPDLOG_EXECUTOR_T critical(format_string_t<Args...> fmt, Args &&... args)
    {
        return log(level::critical, fmt, std::forward<Args>(args)...);
    }

#ifdef SPDLOG_WCHAR_TO_UTF8_SUPPORT
    template<typename... Args>
    SPDLOG_EXECUTOR_T log(source_loc loc, level::level_enum lvl, wformat_string_t<Args...> fmt, Args &&... args)
    {
        return log_(loc, lvl, fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    SPDLOG_EXECUTOR_T log(level::level_enum lvl, wformat_string_t<Args...> fmt, Args &&... args)
    {
        return log(source_loc{}, lvl, fmt, std::forward<Args>(args)...);
    }

    SPDLOG_EXECUTOR_T log(log_clock::time_point log_time, source_loc loc, level::level_enum lvl, wstring_view_t msg)
    {
        bool log_enabled = should_log(lvl);
        bool traceback_enabled = tracer_.enabled();
        if (!log_enabled && !traceback_enabled)
        {
            return;
        }

        memory_buf_t buf;
        details::os::wstr_to_utf8buf(wstring_view_t(msg.data(), msg.size()), buf);
        details::log_msg log_msg(log_time, loc, name_, lvl, string_view_t(buf.data(), buf.size()));
        return log_it_(log_msg, log_enabled, traceback_enabled);
    }

    SPDLOG_EXECUTOR_T log(source_loc loc, level::level_enum lvl, wstring_view_t msg)
    {
        bool log_enabled = should_log(lvl);
        bool traceback_enabled = tracer_.enabled();
        if (!log_enabled && !traceback_enabled)
        {
            return;
        }

        memory_buf_t buf;
        details::os::wstr_to_utf8buf(wstring_view_t(msg.data(), msg.size()), buf);
        details::log_msg log_msg(loc, name_, lvl, string_view_t(buf.data(), buf.size()));
        return log_it_(log_msg, log_enabled, traceback_enabled);
    }

    SPDLOG_EXECUTOR_T log(level::level_enum lvl, wstring_view_t msg)
    {
        return log(source_loc{}, lvl, msg);
    }

    template<typename... Args>
    SPDLOG_EXECUTOR_T trace(wformat_string_t<Args...> fmt, Args &&... args)
    {
        return log(level::trace, fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    SPDLOG_EXECUTOR_T debug(wformat_string_t<Args...> fmt, Args &&... args)
    {
        return log(level::debug, fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    SPDLOG_EXECUTOR_T info(wformat_string_t<Args...> fmt, Args &&... args)
    {
        return log(level::info, fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    SPDLOG_EXECUTOR_T warn(wformat_string_t<Args...> fmt, Args &&... args)
    {
        return log(level::warn, fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    SPDLOG_EXECUTOR_T error(wformat_string_t<Args...> fmt, Args &&... args)
    {
        return log(level::err, fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    SPDLOG_EXECUTOR_T critical(wformat_string_t<Args...> fmt, Args &&... args)
    {
        return log(level::critical, fmt, std::forward<Args>(args)...);
    }
#endif

    template<typename T>
    SPDLOG_EXECUTOR_T trace(const T &msg)
    {
        return log(level::trace, msg);
    }

    template<typename T>
    SPDLOG_EXECUTOR_T debug(const T &msg)
    {
        return log(level::debug, msg);
    }

    template<typename T>
    SPDLOG_EXECUTOR_T info(const T &msg)
    {
        return log(level::info, msg);
    }

    template<typename T>
    SPDLOG_EXECUTOR_T warn(const T &msg)
    {
        return log(level::warn, msg);
    }

    template<typename T>
    SPDLOG_EXECUTOR_T error(const T &msg)
    {
        return log(level::err, msg);
    }

    template<typename T>
    SPDLOG_EXECUTOR_T critical(const T &msg)
    {
        return log(level::critical, msg);
    }

    // return true logging is enabled for the given level.
    bool should_log(level::level_enum msg_level) const
    {
        return msg_level >= level_.load(std::memory_order_relaxed);
    }

    // return true if backtrace logging is enabled.
    bool should_backtrace() const
    {
        return tracer_.enabled();
    }

    void set_level(level::level_enum log_level);

    level::level_enum level() const;

    const std::string &name() const;

    // Indicates the value of the propagate property
    bool propagate() const;
    // Sets the propagate property. Only if propagate is true, log messages are propagated through the hierarchy
    void set_propagate(bool);

    // Gets the hierarchical parent of this logger.
    std::shared_ptr<spdlog::logger> parent() const;

    // set formatting for the sinks in this logger.
    // each sink will get a separate instance of the formatter object.
    void set_formatter(std::unique_ptr<formatter> f);

    // set formatting for the sinks in this logger.
    // equivalent to
    //     set_formatter(make_unique<pattern_formatter>(pattern, time_type))
    // Note: each sink will get a new instance of a formatter object, replacing the old one.
    void set_pattern(std::string pattern, pattern_time_type time_type = pattern_time_type::local);

#ifdef SPDLOG_JSON_LOGGER
    template<class... Args>
    void set_populators(Args &&... args)
    {
        set_formatter(details::make_unique<json_formatter>(populators::make_populator_set(std::forward<Args>(args)...)));
    }
#endif

    // backtrace support.
    // efficiently store all debug/trace messages in a circular buffer until needed for debugging.
    void enable_backtrace(size_t n_messages);
    void disable_backtrace();
    void dump_backtrace();

    // flush functions
    void flush();
    void flush_on(level::level_enum log_level);
    level::level_enum flush_level() const;

    // sinks
    const std::vector<sink_ptr> &sinks() const;

    std::vector<sink_ptr> &sinks();

    // error handler
    void set_error_handler(err_handler);

    // create new logger with same sinks and configuration.
    virtual std::shared_ptr<logger> clone(std::string logger_name);

    void executor_callback(const details::log_msg &log_msg, bool log_enabled, bool traceback_enabled);

protected:
    std::string name_;
    std::vector<sink_ptr> sinks_;
    spdlog::level_t level_{level::info};
    spdlog::level_t flush_level_{level::off};
    err_handler custom_err_handler_{nullptr};
    details::backtracer tracer_;
    bool propagate_{true};

    std::shared_ptr<spdlog::logger> parent_;

    // common implementation for after templated public api has been resolved
    template<typename... Args>
    SPDLOG_EXECUTOR_T log_(source_loc loc, level::level_enum lvl, string_view_t fmt, Args &&... args)
    {
        bool log_enabled = should_log(lvl);
        bool traceback_enabled = tracer_.enabled();
        if (!log_enabled && !traceback_enabled)
        {
            return SPDLOG_EXECUTOR_T{};
        }
        SPDLOG_TRY
        {
            memory_buf_t buf;
#ifdef SPDLOG_USE_STD_FORMAT
            fmt_lib::vformat_to(std::back_inserter(buf), fmt, fmt_lib::make_format_args(std::forward<Args>(args)...));
#else
            fmt::vformat_to(fmt::appender(buf), fmt, fmt::make_format_args(std::forward<Args>(args)...));
#endif

            details::log_msg log_msg(loc, name_, lvl, string_view_t(buf.data(), buf.size()));
            return log_it_(log_msg, log_enabled, traceback_enabled);
        }
        SPDLOG_LOGGER_CATCH(loc)
        return SPDLOG_EXECUTOR_T{};
    }

#ifdef SPDLOG_WCHAR_TO_UTF8_SUPPORT
    template<typename... Args>
    SPDLOG_EXECUTOR_T log_(source_loc loc, level::level_enum lvl, wstring_view_t fmt, Args &&... args)
    {
        bool log_enabled = should_log(lvl);
        bool traceback_enabled = tracer_.enabled();
        if (!log_enabled && !traceback_enabled)
        {
            return SPDLOG_EXECUTOR_T{};
        }
        SPDLOG_TRY
        {
            // format to wmemory_buffer and convert to utf8
            wmemory_buf_t wbuf;
#    ifdef SPDLOG_USE_STD_FORMAT
            fmt_lib::vformat_to(
                std::back_inserter(wbuf), fmt, fmt_lib::make_format_args<fmt_lib::wformat_context>(std::forward<Args>(args)...));
#    else
            fmt::vformat_to(std::back_inserter(wbuf), fmt, fmt::make_format_args<fmt::wformat_context>(std::forward<Args>(args)...));
#    endif

            memory_buf_t buf;
            details::os::wstr_to_utf8buf(wstring_view_t(wbuf.data(), wbuf.size()), buf);
            details::log_msg log_msg(loc, name_, lvl, string_view_t(buf.data(), buf.size()));
            return log_it_(log_msg, log_enabled, traceback_enabled);
        }
        SPDLOG_LOGGER_CATCH(loc)
        return SPDLOG_EXECUTOR_T{};
    }

    // T can be statically converted to wstring_view, and no formatting needed.
    template<class T, typename std::enable_if<std::is_convertible<const T &, spdlog::wstring_view_t>::value, int>::type = 0>
    SPDLOG_EXECUTOR_T log_(source_loc loc, level::level_enum lvl, const T &msg)
    {
        bool log_enabled = should_log(lvl);
        bool traceback_enabled = tracer_.enabled();
        if (!log_enabled && !traceback_enabled)
        {
            return SPDLOG_EXECUTOR_T{};
        }
        SPDLOG_TRY
        {
            memory_buf_t buf;
            details::os::wstr_to_utf8buf(msg, buf);
            details::log_msg log_msg(loc, name_, lvl, string_view_t(buf.data(), buf.size()));
            return log_it_(log_msg, log_enabled, traceback_enabled);
        }
        SPDLOG_LOGGER_CATCH(loc)
        return SPDLOG_EXECUTOR_T{};
    }

#endif // SPDLOG_WCHAR_TO_UTF8_SUPPORT

    // log the given message (if the given log level is high enough),
    // and save backtrace (if backtrace is enabled).
    SPDLOG_EXECUTOR_T log_it_(const details::log_msg &log_msg, bool log_enabled, bool traceback_enabled);
    virtual void sink_it_(const details::log_msg &msg);
    virtual void flush_();
    void dump_backtrace_();
    bool should_flush_(const details::log_msg &msg);

    // handle errors during logging.
    // default handler prints the error to stderr at max rate of 1 message/sec.
    void err_handler_(const std::string &msg);

    void set_parent(std::shared_ptr<spdlog::logger> parent_logger) {
        parent_ = parent_logger;
    }

    // friend class declaration for registry, since it needs to set parent of logger
    friend class spdlog::details::registry;
};

void swap(logger &a, logger &b);

} // namespace spdlog

#ifdef SPDLOG_HEADER_ONLY
#    include "details/executor-inl.h"
#    include "logger-inl.h"
#endif
