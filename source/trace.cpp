#include "trace.hpp"
#include <atomic>
#include <cstdio>
namespace xs {
#ifdef __SWITCH__
static const char* path="sdmc:/switch/xuperswitch/diagnostico.log";
#else
static const char* path="diagnostico.log";
#endif
static std::atomic<unsigned> sequence{0};
void trace_reset(){sequence.store(0);if(FILE* f=std::fopen(path,"w")){std::fputs("XuperSwitch 0.14.1 - etapas; sin datos de cuenta\n",f);std::fclose(f);}}
void trace(const char* stage){if(FILE* f=std::fopen(path,"a")){std::fprintf(f,"%u %s\n",sequence.fetch_add(1)+1,stage);std::fclose(f);}}
}
