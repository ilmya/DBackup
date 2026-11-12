// 定时任务表达式的解析和触发时间计算
// 提供 Cron 表达式解析、时间匹配及下一次触发时间的查询接口。
#pragma once
#include <ctime>
#include <set>
#include <string>

// 持有解析后的五个 Cron 字段并提供匹配和下次触发时间计算。
class CronExpression {
  public:
    // 解析五段 cron 表达式；非法范围或步长会给出错误原因
    bool parse(const std::string& expression, std::string& error);
    // 判断本地时间是否符合已解析的计划
    bool matches(const std::tm& localTime) const;
    // 查找指定时刻之后的下一个匹配时间，超过搜索范围返回 -1
    std::time_t next(std::time_t after, int searchLimitMinutes = 8 * 366 * 24 * 60) const;

  private:
    std::set<int> minute_, hour_, day_, month_, weekday_;
    bool dayWildcard_ = true, weekdayWildcard_ = true;
};
