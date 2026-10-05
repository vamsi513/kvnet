// Unit tests for the command executor. Plain asserts, no test framework.
#include <cstdio>
#include <cstdlib>
#include <string>

#include "../src/protocol.hpp"

static int failures = 0;

#define CHECK_EQ(actual, expected)                                              \
    do {                                                                        \
        std::string a_ = (actual), e_ = (expected);                             \
        if (a_ != e_) {                                                         \
            std::fprintf(stderr, "FAIL line %d: got [%s] want [%s]\n", __LINE__, \
                         a_.c_str(), e_.c_str());                               \
            ++failures;                                                         \
        }                                                                       \
    } while (0)

int main() {
    kvnet::Store s;
    CHECK_EQ(s.execute("PING"), "+PONG\r\n");
    CHECK_EQ(s.execute("ping"), "+PONG\r\n");                    // case-insensitive
    CHECK_EQ(s.execute("SET a 1"), "+OK\r\n");
    CHECK_EQ(s.execute("GET a"), "$1\r\n");
    CHECK_EQ(s.execute("GET missing"), "*-1\r\n");
    CHECK_EQ(s.execute("SET neg -1"), "+OK\r\n");
    CHECK_EQ(s.execute("GET neg"), "$-1\r\n");                  // a hit, not a miss
    CHECK_EQ(s.execute("DEL neg"), ":1\r\n");
    CHECK_EQ(s.execute("GET neg"), "*-1\r\n");                  // now a miss
    CHECK_EQ(s.execute("SET greeting hello world"), "+OK\r\n");  // value keeps spaces
    CHECK_EQ(s.execute("GET greeting"), "$hello world\r\n");
    CHECK_EQ(s.execute("DEL a"), ":1\r\n");
    CHECK_EQ(s.execute("DEL a"), ":0\r\n");
    CHECK_EQ(s.execute("INCR n"), ":1\r\n");                     // missing key starts at 0
    CHECK_EQ(s.execute("INCR n"), ":2\r\n");
    CHECK_EQ(s.execute("SET word abc"), "+OK\r\n");
    CHECK_EQ(s.execute("INCR word"), "-ERR value is not an integer\r\n");
    CHECK_EQ(s.execute("SET n 12abc"), "+OK\r\n");
    CHECK_EQ(s.execute("INCR n"), "-ERR value is not an integer\r\n");
    CHECK_EQ(s.execute(""), "-ERR empty command\r\n");
    CHECK_EQ(s.execute("   "), "-ERR empty command\r\n");
    CHECK_EQ(s.execute("SET onlykey"), "-ERR usage: SET key value\r\n");
    CHECK_EQ(s.execute("GET"), "-ERR usage: GET key\r\n");
    CHECK_EQ(s.execute("NOPE x"), "-ERR unknown command\r\n");
    CHECK_EQ(s.execute("SET big 99999999999999999999"), "+OK\r\n");
    CHECK_EQ(s.execute("INCR big"), "-ERR value is not an integer\r\n");  // too long to parse
    CHECK_EQ(s.execute("SET max 9223372036854775807"), "+OK\r\n");
    CHECK_EQ(s.execute("INCR max"), "-ERR value out of range\r\n");       // would overflow
    CHECK_EQ(s.execute("GET max"), "$9223372036854775807\r\n");           // left unchanged

    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::puts("all protocol tests passed");
    return 0;
}
