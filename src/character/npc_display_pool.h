#pragma once
#include <algorithm>
#include <cstddef>
#include <memory>
#include <vector>

// Game-thread ownership ledger. A failed native destruction must stay recorded;
// eviction never silently frees a live component or exceeds the resident limit.
namespace NpcDisplayPool {
template<class Bundle,size_t Limit=4> class Store {
    std::vector<std::unique_ptr<Bundle>> items_;
public:
    auto& Items(){return items_;}
    const auto& Items()const{return items_;}
    bool Full()const{return items_.size()>=Limit;}
    template<class Drop> bool MakeRoom(Drop drop){
        if(!Full())return true;
        const auto oldest=std::min_element(items_.begin(),items_.end(),[](const auto& a,const auto& b){return a->used<b->used;});
        if(!drop(**oldest))return false;items_.erase(oldest);return true;
    }
    bool Put(std::unique_ptr<Bundle>& bundle){if(!bundle||Full())return false;items_.push_back(std::move(bundle));return true;}
    template<class Match> std::unique_ptr<Bundle> Take(Match match){
        for(auto it=items_.begin();it!=items_.end();++it)if(match(**it)){auto found=std::move(*it);items_.erase(it);return found;}return {};
    }
    template<class Keep,class Drop> void Prune(Keep keep,Drop drop){
        for(auto it=items_.begin();it!=items_.end();)if(!keep(**it)&&drop(**it))it=items_.erase(it);else ++it;
    }
    template<class Drop> bool Clear(Drop drop){Prune([](const Bundle&){return false;},drop);return items_.empty();}
};
}
