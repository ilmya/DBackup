#include "automation/cron.h"
#include <sstream>
#include <vector>

namespace {
bool parseNumber(const std::string &text, int min, int max, int &value) {
    try { size_t used=0; int parsed=std::stoi(text,&used); if(used!=text.size()||parsed<min||parsed>max)return false;value=parsed;return true; } catch (...) { return false; }
}
bool parseField(const std::string &text,int min,int max,std::set<int>&values,std::string&error){
    values.clear();std::stringstream parts(text);std::string part;
    while(std::getline(parts,part,',')){int step=1;size_t slash=part.find('/');if(slash!=std::string::npos){if(!parseNumber(part.substr(slash+1),1,max-min+1,step)){error="Cron 步长无效: "+part;return false;}part=part.substr(0,slash);}int from=min,to=max;if(part!="*"){size_t dash=part.find('-');if(dash==std::string::npos){if(!parseNumber(part,min,max,from)){error="Cron 数值无效: "+part;return false;}to=from;}else if(!parseNumber(part.substr(0,dash),min,max,from)||!parseNumber(part.substr(dash+1),min,max,to)||from>to){error="Cron 范围无效: "+part;return false;}}for(int i=from;i<=to;i+=step)values.insert(i);}
    return !values.empty();
}
}
bool CronExpression::parse(const std::string&e,std::string&error){std::stringstream in(e);std::vector<std::string>f;std::string v;while(in>>v)f.push_back(v);if(f.size()!=5){error="Cron 必须包含分钟、小时、日期、月份、星期五个字段";return false;}std::set<int>minute,hour,day,month,weekday;if(!parseField(f[0],0,59,minute,error)||!parseField(f[1],0,23,hour,error)||!parseField(f[2],1,31,day,error)||!parseField(f[3],1,12,month,error)||!parseField(f[4],0,6,weekday,error))return false;minute_=std::move(minute);hour_=std::move(hour);day_=std::move(day);month_=std::move(month);weekday_=std::move(weekday);return true;}
bool CronExpression::matches(const std::tm&t)const{return minute_.count(t.tm_min)&&hour_.count(t.tm_hour)&&day_.count(t.tm_mday)&&month_.count(t.tm_mon+1)&&weekday_.count(t.tm_wday);}
std::time_t CronExpression::next(std::time_t after,int limit)const{std::time_t value=after-after%60+60;for(int i=0;i<limit;++i,value+=60){std::tm local={};localtime_s(&local,&value);if(matches(local))return value;}return static_cast<std::time_t>(-1);}
