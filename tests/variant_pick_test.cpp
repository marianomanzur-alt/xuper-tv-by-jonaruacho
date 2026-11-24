#include "variant_pick.hpp"
#include <cassert>
#include <iostream>
int main(){
    using namespace xs;
    const std::vector<std::string> v{"1. 1080p / Spanish","2. 1080p / Japanese","3. 720p / Japanese","4. 720p / Spanish"};
    assert(variant_key("2. 1080p / Japanese")=="1080p / japanese");
    assert(variant_key("  720P / Japanese ")=="720p / japanese");
    assert(variant_key("sin numero")=="sin numero");
    assert(pick_variant(v,"720p / japanese")==2);                 // misma calidad y audio
    assert(pick_variant(v,"3. 720p / Japanese")==2);              // aunque venga con numero
    assert(pick_variant(v,"480p / japanese")==1);                 // otra calidad: el mismo audio, primera
    assert(pick_variant(v,"1080p / german")==-1);                 // audio que no existe: no se cambia
    assert(pick_variant(v,"")==-1&&pick_variant({},"720p / japanese")==-1);
    const std::vector<std::string> plain{"1. 720p","2. 1080p"};   // sin audio informado: solo coincidencia exacta
    assert(pick_variant(plain,"1080p")==1&&pick_variant(plain,"480p")==-1);
    assert(variant_key(std::string("a\x01""b"))=="a b");
    std::cout<<"variant pick passed\n";
}
