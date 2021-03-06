/**
* Copyright (C) Mellanox Technologies Ltd. 2001-2012.  ALL RIGHTS RESERVED.
*
* $COPYRIGHT$
* $HEADER$
*/

#include "test_helpers.h"

extern "C" {
#include <ucs/sys/math.h>
#include <ucs/sys/sys.h>
#include <ucs/time/time.h>
}

namespace ucs {

int test_time_multiplier()
{
    int factor = 1;
#if _BullseyeCoverage
    factor *= 10;
#endif
    if (RUNNING_ON_VALGRIND) {
        factor *= 20;
    }
    return factor;
}

std::ostream& operator<<(std::ostream& os, const std::vector<char>& vec) {
    static const size_t LIMIT = 100;
    size_t i = 0;
    for (std::vector<char>::const_iterator iter = vec.begin(); iter != vec.end(); ++iter) {
        if (i >= LIMIT) {
            os << "...";
            break;
        }
        int n = static_cast<unsigned char>(*iter);
        os << "[" << i << "]=" << n << " ";
        ++i;
    }
    return os << std::endl;
}

scoped_setenv::scoped_setenv(const char *name, const char *value) : m_name(name) {
    if (getenv(name)) {
        m_old_value = getenv(m_name.c_str());
    }
    setenv(m_name.c_str(), value, 1);
}

scoped_setenv::~scoped_setenv() {
    if (!m_old_value.empty()) {
        setenv(m_name.c_str(), m_old_value.c_str(), 1);
    } else {
        unsetenv(m_name.c_str());
    }
}

void safe_usleep(double usec) {
    ucs_time_t current_time = ucs_get_time();
    ucs_time_t end_time = current_time + ucs_time_from_usec(usec);

    while (current_time < end_time) {
        usleep((long)ucs_time_to_usec(end_time - current_time));
        current_time = ucs_get_time();
    }
}


namespace detail {

message_stream::message_stream(const std::string& title) {
    static const char PADDING[] = "          ";
    static const size_t WIDTH = strlen(PADDING);

    std::cout <<  "[";
    std::cout.write(PADDING, ucs_max(WIDTH - 1, title.length()) - title.length());
    std::cout << title << " ] ";
}

message_stream::~message_stream() {
    std::cout << std::endl;
}

} // detail

} // ucs
