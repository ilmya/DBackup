#pragma once
#include <ctime>
#include <set>
#include <string>

class CronExpression {
 public:
    bool parse(const std::string &expression, std::string &error);
    bool matches(const std::tm &localTime) const;
    std::time_t next(std::time_t after, int searchLimitMinutes = 366 * 24 * 60) const;
 private:
    std::set<int> minute_, hour_, day_, month_, weekday_;
};
