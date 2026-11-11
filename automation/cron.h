#pragma once
#include <ctime>
#include <set>
#include <string>

class CronExpression {
  public:
    // 解析五段 cron 表达式；非法范围或步长会给出错误原因。
    bool parse(const std::string& expression, std::string& error);
    // 判断本地时间是否符合已解析的计划。
    bool matches(const std::tm& localTime) const;
    // 查找指定时刻之后的下一个匹配时间，超过搜索范围返回 -1。
    std::time_t next(std::time_t after, int searchLimitMinutes = 8 * 366 * 24 * 60) const;

  private:
    std::set<int> minute_, hour_, day_, month_, weekday_;
    bool dayWildcard_ = true, weekdayWildcard_ = true;
};
