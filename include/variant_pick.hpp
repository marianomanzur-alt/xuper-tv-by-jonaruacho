#pragma once
// Recordar la "fuente" (calidad / idioma de audio) elegida para aplicarla al cambiar de capitulo.
// Las etiquetas del servicio tienen la forma "2. 720p / Japanese": numero, calidad y audio.
#include <algorithm>
#include <cctype>
#include <string>
#include <vector>
namespace xs {
inline std::string variant_key(const std::string& label){
    std::string s=label;size_t i=0;while(i<s.size()&&std::isdigit(static_cast<unsigned char>(s[i])))++i;
    if(i>0&&i+1<s.size()&&s[i]=='.'&&s[i+1]==' ')s=s.substr(i+2);
    for(char& c:s)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    while(!s.empty()&&s.front()==' ')s.erase(s.begin());
    while(!s.empty()&&s.back()==' ')s.pop_back();
    if(s.size()>80)s.resize(80);
    for(char& c:s)if(static_cast<unsigned char>(c)<32||c==127)c=' ';
    return s;
}
// Devuelve el indice de la fuente que mejor coincide con la recordada, o -1.
// 1) misma calidad y audio; 2) mismo audio (la primera, que el servicio ordena por calidad).
inline int pick_variant(const std::vector<std::string>& variants,const std::string& wanted){
    if(wanted.empty()||variants.empty())return -1;
    const std::string want=variant_key(wanted);
    auto audio_of=[](const std::string& key){const size_t cut=key.find(" / ");return cut==std::string::npos?std::string():key.substr(cut+3);};
    for(size_t i=0;i<variants.size();++i)if(variant_key(variants[i])==want)return static_cast<int>(i);
    const std::string audio=audio_of(want);
    if(audio.empty())return -1;
    for(size_t i=0;i<variants.size();++i)if(audio_of(variant_key(variants[i]))==audio)return static_cast<int>(i);
    return -1;
}
}
