#pragma once
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <array>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <stdexcept>

// User-space hardware counters for only the execution thread. Compiler thread
// work is excluded (inherit=0). Enable/reset at marker start, disable/read at end.
class Pmu {
    std::array<int,4> fds{{-1,-1,-1,-1}};
public:
    struct Sample { uint64_t enabled=0,running=0; std::array<uint64_t,4> value{}; };
    std::array<Sample,6> samples;
    uint64_t previous_enabled=0,previous_running=0;
    bool enabled = std::getenv("NATIVE_PMU") != nullptr;
    Pmu() {
        if(!enabled)return;
        const uint64_t events[]={PERF_COUNT_HW_CPU_CYCLES,PERF_COUNT_HW_INSTRUCTIONS,PERF_COUNT_HW_BRANCH_INSTRUCTIONS,PERF_COUNT_HW_BRANCH_MISSES};
        for(unsigned i=0;i<4;++i){
            perf_event_attr attr{};attr.size=sizeof attr;attr.type=PERF_TYPE_HARDWARE;attr.config=events[i];
            attr.disabled=1;attr.exclude_kernel=1;attr.exclude_hv=1;
            attr.read_format=PERF_FORMAT_GROUP|PERF_FORMAT_TOTAL_TIME_ENABLED|PERF_FORMAT_TOTAL_TIME_RUNNING;
            fds[i]=syscall(SYS_perf_event_open,&attr,0,-1,i?fds[0]:-1,0);
            if(fds[i]<0)throw std::runtime_error("perf_event_open failed");
        }
    }
    ~Pmu(){for(int fd:fds)if(fd>=0)close(fd);}
    void start(){if(enabled){ioctl(fds[0],PERF_EVENT_IOC_RESET,PERF_IOC_FLAG_GROUP);ioctl(fds[0],PERF_EVENT_IOC_ENABLE,PERF_IOC_FLAG_GROUP);}}
    void stop(unsigned sample){if(enabled){
        ioctl(fds[0],PERF_EVENT_IOC_DISABLE,PERF_IOC_FLAG_GROUP);
        uint64_t data[7];if(::read(fds[0],data,sizeof data)!=sizeof data||data[0]!=4)throw std::runtime_error("perf group read failed");
        samples[sample].enabled=data[1]-previous_enabled;samples[sample].running=data[2]-previous_running;
        previous_enabled=data[1];previous_running=data[2];
        std::memcpy(samples[sample].value.data(),data+3,4*sizeof(uint64_t));
    }}
    void print(){std::printf("],\"pmu\":[");
        if(enabled)for(unsigned i=0;i<6;++i){const auto&s=samples[i];std::printf("%s{\"enabled\":%llu,\"running\":%llu,\"cycles\":%llu,\"instructions\":%llu,\"branches\":%llu,\"branchMisses\":%llu}",i?",":"",(unsigned long long)s.enabled,(unsigned long long)s.running,(unsigned long long)s.value[0],(unsigned long long)s.value[1],(unsigned long long)s.value[2],(unsigned long long)s.value[3]);}
        std::printf("]}\n");
    }
};
