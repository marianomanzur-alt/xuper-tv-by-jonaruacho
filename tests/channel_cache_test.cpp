#include "channel_cache.hpp"
#include <cassert>
#include <cstdio>
#include <iostream>
using namespace xs;
static Entry channel(int column,const char* title,const char* code,const char* art){Entry e;e.kind=EntryKind::Channel;e.column_id=column;e.title=title;e.channel_code=code;e.artwork=art;e.artwork_cache=artwork_key(art);return e;}
int main(){
    const std::string path="channel_cache_test.json";std::remove(path.c_str());std::remove((path+".bak").c_str());
    ChannelIndex index;
    index[7]={channel(7,"Canal Uno","c1","https://example.com/a.png"),channel(7,"Canal \xC3\xB1" "and\xC3\xBA","c2","")};
    index[9]={channel(9,"Deportes","c3","https://example.com/b.jpg")};
    ChannelIndex out;
    assert(!load_channel_index(path,3600,out));                         // sin archivo
    assert(save_channel_index(path,index,1000));
    assert(save_channel_index(path,index,1000));                        // reemplazo sobre archivo existente
    assert(load_channel_index(path,3600,out,1000+60));
    assert(out.size()==2&&out[7].size()==2&&out[9].size()==1);
    assert(out[7][1].title=="Canal \xC3\xB1" "and\xC3\xBA"&&out[7][0].channel_code=="c1"&&out[7][0].column_id==7);
    assert(out[7][0].kind==EntryKind::Channel&&out[7][0].artwork_cache==artwork_key("https://example.com/a.png"));
    assert(out[7][1].artwork.empty());
    ChannelIndex old;
    assert(!load_channel_index(path,3600,old,1000+3601));               // vencido
    assert(!load_channel_index(path,3600,old,500));                     // fecha en el futuro
    {std::ofstream f(path,std::ios::trunc);f<<"{ esto no es json";}
    assert(!load_channel_index(path,3600,old,1000));                    // corrupto
    {std::ofstream f(path,std::ios::trunc);f<<"{\"saved\":1000,\"categories\":{\"x\":[[\"a\",\"b\"]],\"5\":[[\"ok\",\"code\"],[1,2],[\"\",\"z\"],\"bad\"]}}";}
    assert(load_channel_index(path,3600,old,1000)&&old.size()==1&&old[5].size()==1&&old[5][0].title=="ok"); // filtra filas invalidas
    std::remove(path.c_str());std::remove((path+".bak").c_str());std::remove((path+".tmp").c_str());
    std::cout<<"channel_cache_test OK\n";
}
