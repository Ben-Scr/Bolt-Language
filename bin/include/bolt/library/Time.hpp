#pragma once
#include "Exceptions.hpp"
#include "../Std.hpp"
#include <chrono>
#include <cerrno>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>

namespace bolt::library {
    class TimeSpan {
        std::int64_t ticks_=0;
        static std::int64_t scaled(double value,double scale) {
            if(!std::isfinite(value))throw ArgumentException("Duration must be finite.");
            auto v=std::round(static_cast<long double>(value)*scale);
            const auto limit=std::ldexp(1.0L,63);
            if(!std::isfinite(v)||v<-limit||v>=limit)throw OverflowException("Duration is outside the supported range.");
            return static_cast<std::int64_t>(v);
        }
        public:
        static constexpr std::int64_t TicksPerSecond=10000000;
        TimeSpan()=default;
        explicit TimeSpan(std::int64_t ticks):ticks_(ticks) {
        }
        TimeSpan(std::int32_t hours,std::int32_t minutes,std::int32_t seconds):TimeSpan(0,hours,minutes,seconds,0) {
        }
        TimeSpan(std::int32_t days,std::int32_t hours,std::int32_t minutes,std::int32_t seconds,std::int32_t milliseconds=0) {
            auto seconds64=native::add<std::int64_t>(native::add<std::int64_t>(native::mul<std::int64_t>(days,86400),native::mul<std::int64_t>(hours,3600)),native::add<std::int64_t>(native::mul<std::int64_t>(minutes,60),seconds));
            ticks_=native::add<std::int64_t>(native::mul<std::int64_t>(seconds64,TicksPerSecond),native::mul<std::int64_t>(milliseconds,10000));
        }
        std::int64_t Ticks()const {
            return ticks_;
        }
        double TotalDays()const {
            return static_cast<double>(ticks_)/864000000000.;
        }
        double TotalHours()const {
            return static_cast<double>(ticks_)/36000000000.;
        }
        double TotalMinutes()const {
            return static_cast<double>(ticks_)/600000000.;
        }
        double TotalSeconds()const {
            return static_cast<double>(ticks_)/10000000.;
        }
        double TotalMilliseconds()const {
            return static_cast<double>(ticks_)/10000.;
        }
        double TotalMicroseconds()const {
            return static_cast<double>(ticks_)/10.;
        }
        std::int32_t Days()const {
            return static_cast<std::int32_t>(ticks_/864000000000LL);
        }
        std::int32_t Hours()const {
            return static_cast<std::int32_t>((ticks_/36000000000LL)%24);
        }
        std::int32_t Minutes()const {
            return static_cast<std::int32_t>((ticks_/600000000LL)%60);
        }
        std::int32_t Seconds()const {
            return static_cast<std::int32_t>((ticks_/10000000LL)%60);
        }
        std::int32_t Milliseconds()const {
            return static_cast<std::int32_t>((ticks_/10000)%1000);
        }
        static TimeSpan Zero() {
            return {
            };
        }
        static TimeSpan FromTicks(std::int64_t v) {
            return TimeSpan(v);
        }
        static TimeSpan FromDays(double v) {
            return TimeSpan(scaled(v,864000000000.));
        }
        static TimeSpan FromHours(double v) {
            return TimeSpan(scaled(v,36000000000.));
        }
        static TimeSpan FromMinutes(double v) {
            return TimeSpan(scaled(v,600000000.));
        }
        static TimeSpan FromSeconds(double v) {
            return TimeSpan(scaled(v,10000000.));
        }
        static TimeSpan FromMilliseconds(double v) {
            return TimeSpan(scaled(v,10000.));
        }
        static TimeSpan FromMicroseconds(double v) {
            return TimeSpan(scaled(v,10.));
        }
        TimeSpan Add(TimeSpan other)const {
            return TimeSpan(native::add<std::int64_t>(ticks_,other.ticks_));
        }
        TimeSpan Subtract(TimeSpan other)const {
            return TimeSpan(native::sub<std::int64_t>(ticks_,other.ticks_));
        }
        TimeSpan Duration()const {
            return ticks_>=0?*this:Negate();
        }
        TimeSpan Negate()const {
            return TimeSpan(native::sub<std::int64_t>(0,ticks_));
        }
        native::String ToString()const {
            const auto magnitude=ticks_<0?std::uint64_t {
                0
            }
            -static_cast<std::uint64_t>(ticks_):static_cast<std::uint64_t>(ticks_);
            const auto days=magnitude/864000000000ULL;
            std::ostringstream out;
            out.imbue(std::locale::classic());
            if(ticks_<0)out<<'-';
            if(days)out<<days<<'.';
            out<<std::setfill('0')<<std::setw(2)<<(magnitude/36000000000ULL)%24<<':'<<std::setw(2)<<(magnitude/600000000ULL)%60<<':'<<std::setw(2)<<(magnitude/10000000ULL)%60<<'.'<<std::setw(7)<<magnitude%10000000ULL;
            return out.str();
        }
        static TimeSpan Sum(TimeSpan a,TimeSpan b) {
            return a.Add(b);
        }
        static TimeSpan Difference(TimeSpan a,TimeSpan b) {
            return a.Subtract(b);
        }
        static TimeSpan Negative(TimeSpan a) {
            return a.Negate();
        }
        static bool Less(TimeSpan a,TimeSpan b) {
            return a.ticks_<b.ticks_;
        }
        static bool Greater(TimeSpan a,TimeSpan b) {
            return a.ticks_>b.ticks_;
        }
        static bool LessEqual(TimeSpan a,TimeSpan b) {
            return a.ticks_<=b.ticks_;
        }
        static bool GreaterEqual(TimeSpan a,TimeSpan b) {
            return a.ticks_>=b.ticks_;
        }
        std::string BoltFormat()const {
            return ToString();
        }
        auto operator<=>(const TimeSpan&)const=default;
        std::size_t BoltHash()const {
            return std::hash<std::int64_t> {
            }
            (ticks_);
        }
    };

    // Ticks are 100 ns since the Unix epoch (1970-01-01T00:00:00Z), not the .NET
    // year-1 epoch. Calendar methods use UTC unless ToLocalTime() is selected.
    class DateTime {
        std::int64_t ticks_=0;
        bool local_=false;
        using TicksDuration=std::chrono::duration<std::int64_t,std::ratio<1,10000000>>;
        std::tm calendar() const {
            using namespace std::chrono;
            const auto seconds = floor<std::chrono::seconds>(TicksDuration(ticks_));
            std::tm result{};
            if (!local_) {
                // Calendar arithmetic is independent of time_t/gmtime limits,
                // including Windows' restricted C-runtime calendar range.
                const sys_seconds instant{seconds};
                const auto day = floor<std::chrono::days>(instant);
                const year_month_day date{day};
                const hh_mm_ss time{instant - day};
                result.tm_year = static_cast<int>(date.year()) - 1900;
                result.tm_mon = static_cast<int>(static_cast<unsigned>(date.month())) - 1;
                result.tm_mday = static_cast<int>(static_cast<unsigned>(date.day()));
                result.tm_hour = static_cast<int>(time.hours().count());
                result.tm_min = static_cast<int>(time.minutes().count());
                result.tm_sec = static_cast<int>(time.seconds().count());
                result.tm_wday = static_cast<int>(weekday{day}.c_encoding());
                result.tm_yday = static_cast<int>((day - sys_days{date.year()/January/1}).count());
                return result;
            }
            const auto epoch = native::cast<std::time_t>(seconds.count());
#ifdef _WIN32
            if (localtime_s(&result, &epoch))
#else
            if (!localtime_r(&epoch, &result))
#endif
                throw ArgumentOutOfRangeException("Local DateTime is outside the platform timezone range.");
            if (result.tm_year < -1899 || result.tm_year > 8099)
                throw ArgumentOutOfRangeException("Local DateTime must lie in years 1 through 9999.");
            return result;
        }
        public:
        DateTime()=default;
        explicit DateTime(std::int64_t ticks):ticks_(ticks) {
            if(ticks<-621355968000000000LL||ticks>2534023007999999999LL)throw ArgumentOutOfRangeException("DateTime must lie between UTC years 1 and 9999.");
        }
        DateTime(std::int32_t year,std::int32_t month,std::int32_t day,std::int32_t hour=0,std::int32_t minute=0,std::int32_t second=0,std::int32_t millisecond=0) {
            if(year<1||year>9999||hour<0||hour>23||minute<0||minute>59||second<0||second>59||millisecond<0||millisecond>999)throw ArgumentOutOfRangeException("Invalid UTC calendar components.");
            const std::chrono::year_month_day ymd {
                std::chrono::year {
                    year
                },std::chrono::month {
                    static_cast<unsigned>(month)
                },std::chrono::day {
                    static_cast<unsigned>(day)
                }
            };
            if(!ymd.ok())throw ArgumentOutOfRangeException("Invalid calendar date.");
            auto days=std::chrono::sys_days {
                ymd
            }
            .time_since_epoch().count();
            ticks_=native::add<std::int64_t>(native::mul<std::int64_t>(days,864000000000LL),TimeSpan(0,hour,minute,second,millisecond).Ticks());
        }
        static DateTime UtcNow() {
            return DateTime(std::chrono::duration_cast<TicksDuration>(std::chrono::system_clock::now().time_since_epoch()).count());
        }
        static DateTime Now() {
            auto v=UtcNow();
            v.local_=true;
            return v;
        }
        static DateTime Today() {
            auto parts=Now().calendar();
            parts.tm_hour=0;
            parts.tm_min=0;
            parts.tm_sec=0;
            parts.tm_isdst=-1;
            errno=0;
            auto seconds=std::mktime(&parts);
            if(seconds==std::time_t(-1)&&errno)throw ArgumentOutOfRangeException("Cannot represent today's local midnight.");
            return FromUnixTimeSeconds(native::cast<std::int64_t>(seconds)).ToLocalTime();
        }
        static DateTime UnixEpoch() {
            return DateTime();
        }
        static DateTime FromUnixTimeSeconds(std::int64_t seconds) {
            return DateTime(native::mul<std::int64_t>(seconds,TimeSpan::TicksPerSecond));
        }
        static DateTime FromUnixTimeMilliseconds(std::int64_t ms) {
            return DateTime(native::mul<std::int64_t>(ms,10000));
        }
        std::int64_t ToUnixTimeSeconds()const {
            return std::chrono::floor<std::chrono::seconds>(TicksDuration(ticks_)).count();
        }
        std::int64_t ToUnixTimeMilliseconds()const {
            return std::chrono::floor<std::chrono::milliseconds>(TicksDuration(ticks_)).count();
        }
        std::int64_t Ticks()const {
            return ticks_;
        }
        std::int32_t Year()const {
            return calendar().tm_year+1900;
        }
        std::int32_t Month()const {
            return calendar().tm_mon+1;
        }
        std::int32_t Day()const {
            return calendar().tm_mday;
        }
        std::int32_t Hour()const {
            return calendar().tm_hour;
        }
        std::int32_t Minute()const {
            return calendar().tm_min;
        }
        std::int32_t Second()const {
            return calendar().tm_sec;
        }
        std::int32_t Millisecond()const {
            auto fraction=ticks_%10000000;
            if(fraction<0)fraction+=10000000;
            return static_cast<std::int32_t>(fraction/10000);
        }
        std::int32_t DayOfWeek()const {
            return calendar().tm_wday;
        }
        std::int32_t DayOfYear()const {
            return calendar().tm_yday+1;
        }
        bool IsUtc()const {
            return !local_;
        }
        DateTime ToUniversalTime()const {
            auto v=*this;
            v.local_=false;
            return v;
        }
        DateTime ToLocalTime()const {
            auto v=*this;
            v.local_=true;
            return v;
        }
        DateTime Add(TimeSpan value)const {
            auto v=DateTime(native::add<std::int64_t>(ticks_,value.Ticks()));
            v.local_=local_;
            return v;
        }
        DateTime AddDays(double value)const {
            return Add(TimeSpan::FromDays(value));
        }
        DateTime AddHours(double value)const {
            return Add(TimeSpan::FromHours(value));
        }
        DateTime AddMinutes(double value)const {
            return Add(TimeSpan::FromMinutes(value));
        }
        DateTime AddSeconds(double value)const {
            return Add(TimeSpan::FromSeconds(value));
        }
        DateTime AddMilliseconds(double value)const {
            return Add(TimeSpan::FromMilliseconds(value));
        }
        TimeSpan Subtract(DateTime value)const {
            return TimeSpan(native::sub<std::int64_t>(ticks_,value.ticks_));
        }
        native::String ToString()const {
            const auto tm=calendar();
            std::ostringstream out;
            out.imbue(std::locale::classic());
            out << std::setfill('0') << std::setw(4) << tm.tm_year+1900
                << '-' << std::setw(2) << tm.tm_mon+1 << '-' << std::setw(2) << tm.tm_mday
                << 'T' << std::setw(2) << tm.tm_hour << ':' << std::setw(2) << tm.tm_min
                << ':' << std::setw(2) << tm.tm_sec << '.' << std::setw(7)
                << ((ticks_%10000000+10000000)%10000000);
            if(!local_)out<<'Z';
            else {
                const auto ymd=std::chrono::year_month_day {
                    std::chrono::year {
                        tm.tm_year+1900
                    },std::chrono::month {
                        static_cast<unsigned>(tm.tm_mon+1)
                    },std::chrono::day {
                        static_cast<unsigned>(tm.tm_mday)
                    }
                };
                const auto wall=std::chrono::sys_days {
                    ymd
                }
                .time_since_epoch().count()*86400+tm.tm_hour*3600+tm.tm_min*60+tm.tm_sec;
                const auto offset=wall-ToUnixTimeSeconds();
                const auto magnitude=offset<0?-offset:offset;
                out<<(offset<0?'-':'+')<<std::setw(2)<<magnitude/3600<<':'<<std::setw(2)<<(magnitude/60)%60;
                if(magnitude%60)out<<':'<<std::setw(2)<<magnitude%60;
            }
            return out.str();
        }
        static DateTime Parse(const native::String& text) {
            // A deterministic ISO-8601 subset, accepting UTC and numeric offsets.
            std::string_view s(text.data(),text.size());
            auto number=[&](std::size_t at,std::size_t count) {
                if(at+count>s.size())throw FormatException("Expected ISO-8601 date/time.");
                int n=0;
                for(std::size_t i=at;i<at+count;++i) {
                    if(s[i]<'0'||s[i]>'9')throw FormatException("Expected decimal calendar components.");
                    n=n*10+(s[i]-'0');
                }
                return n;
            };
            if(s.size()<10||s[4]!='-'||s[7]!='-')throw FormatException("Expected YYYY-MM-DD or an ISO-8601 timestamp.");
            auto year=number(0,4),month=number(5,2),day=number(8,2);
            int hour=0,minute=0,second=0;
            std::size_t pos=10;
            if(pos<s.size()&&(s[pos]=='T'||s[pos]==' ')) {
                if(s.size()<19||s[13]!=':'||s[16]!=':')throw FormatException("Expected HH:MM:SS.");
                hour=number(11,2);
                minute=number(14,2);
                second=number(17,2);
                pos=19;
            }
            std::int64_t fraction=0;
            if(pos<s.size()&&s[pos]=='.') {
                ++pos;
                std::size_t n=0;
                while(pos<s.size()&&s[pos]>='0'&&s[pos]<='9') {
                    if(n>=7)throw FormatException("DateTime supports at most seven fractional digits.");
                    fraction=fraction*10+(s[pos++]-'0');
                    ++n;
                }
                if(!n)throw FormatException("Missing fractional digits.");
                while(n++<7)fraction*=10;
            }
            int offset=0;
            if(pos<s.size()&&s[pos]=='Z')++pos;
            else if(pos<s.size()&&(s[pos]=='+'||s[pos]=='-')) {
                int sign=s[pos++]=='+'?1:-1;
                if(pos+5>s.size()||s[pos+2]!=':')throw FormatException("Expected numeric timezone offset ±HH:MM.");
                auto h=number(pos,2),m=number(pos+3,2);
                if(h>23||m>59)throw FormatException("Invalid timezone offset.");
                offset=sign*(h*3600+m*60);
                pos+=5;
                if(pos<s.size()&&s[pos]==':') {
                    auto seconds=number(pos+1,2);
                    if(seconds>59)throw FormatException("Invalid timezone offset seconds.");
                    offset+=sign*seconds;
                    pos+=3;
                }
            }
            if(pos!=s.size())throw FormatException("Unexpected characters after DateTime.");
            try {
                return DateTime(year,month,day,hour,minute,second).Add(TimeSpan(fraction)).AddSeconds(-offset);
            }catch(const ArgumentException& e) {
                throw FormatException(e.Message());
            }
        }
        static DateTime Plus(DateTime a,TimeSpan b) {
            return a.Add(b);
        }
        static DateTime Minus(DateTime a,TimeSpan b) {
            return a.Add(b.Negate());
        }
        static TimeSpan Difference(DateTime a,DateTime b) {
            return a.Subtract(b);
        }
        static bool Less(DateTime a,DateTime b) {
            return a.ticks_<b.ticks_;
        }
        static bool Greater(DateTime a,DateTime b) {
            return a.ticks_>b.ticks_;
        }
        static bool LessEqual(DateTime a,DateTime b) {
            return a.ticks_<=b.ticks_;
        }
        static bool GreaterEqual(DateTime a,DateTime b) {
            return a.ticks_>=b.ticks_;
        }
        std::string BoltFormat()const {
            return ToString();
        }
        bool operator==(const DateTime& other)const {
            return ticks_==other.ticks_;
        }
        auto operator<=>(const DateTime& other)const {
            return ticks_<=>other.ticks_;
        }
        std::size_t BoltHash()const {
            return std::hash<std::int64_t> {
            }
            (ticks_);
        }
    };

    class Stopwatch {
        using Clock=std::chrono::steady_clock;
        Clock::time_point started_ {
        };
        Clock::duration elapsed_ {
        };
        bool running_=false;
        Clock::duration elapsed()const {
            return elapsed_+(running_?Clock::now()-started_:Clock::duration::zero());
        }
        public:
        Stopwatch()=default;
        bool operator==(const Stopwatch&)const=default;
        std::size_t BoltHash()const {
            return native::hash_combine(std::hash<std::int64_t> {
            }
            (static_cast<std::int64_t>(started_.time_since_epoch().count())),native::hash_combine(std::hash<std::int64_t> {
            }
            (static_cast<std::int64_t>(elapsed_.count())),running_));
        }
        void Start() {
            elapsed_=Clock::duration::zero();
            started_=Clock::now();
            running_=true;
        }
        void Pause() {
            if(running_) {
                elapsed_+=Clock::now()-started_;
                running_=false;
            }
        }
        void Stop() {
            Pause();
        }
        void Unpause() {
            if(!running_) {
                started_=Clock::now();
                running_=true;
            }
        }
        void Reset() {
            elapsed_=Clock::duration::zero();
            running_=false;
        }
        void Restart() {
            Start();
        }
        bool IsRunning()const {
            return running_;
        }
        TimeSpan Elapsed()const {
            return TimeSpan(std::chrono::duration_cast<std::chrono::duration<std::int64_t,std::ratio<1,10000000>>>(elapsed()).count());
        }
        std::int64_t ElapsedMilliseconds()const {
            return std::chrono::duration_cast<std::chrono::milliseconds>(elapsed()).count();
        }
        std::int64_t ElapsedTicks()const {
            return Elapsed().Ticks();
        }
        double ElapsedSeconds()const {
            return std::chrono::duration<double>(elapsed()).count();
        }
        static Stopwatch StartNew() {
            Stopwatch result;
            result.Start();
            return result;
        }
    };
}
