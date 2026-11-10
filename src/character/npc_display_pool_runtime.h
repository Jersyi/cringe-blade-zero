// Included inside npc_retarget.cpp's private game-thread namespace.
bool PoolEligible(int row){
    if(row<0||static_cast<size_t>(row)>=std::size(kProfiles))return false;
    // Mounts own an extra animation puppet and loop leases. Keep their existing
    // complete teardown until that lifetime can be cached together with the rider.
    for(const auto& mount:kMountLoops)if(!std::strcmp(mount.skeleton,kProfiles[row].skeleton))return false;
    return kProfiles[row].part_count<=48;
}
bool PoolValid(const DisplayBundle& b){
    if(b.broken||!Same(b.pawn)||!Same(b.source)||!Same(b.source_asset)||!Same(b.world)||!Same(b.donor)||!Same(b.target)||!Same(b.body)||!Same(b.copy_class))return false;
    if(SkelMesh(b.source.object)!=b.source_asset.object||SkelMesh(b.body.object)!=b.target.object||AnimClass(b.body.object)!=b.copy_class.object||Property<Object*>(b.body.object,"AttachParent")!=b.source.object)return false;
    for(const auto& id:b.owned)if(!Same(id))return false;
    for(const auto& id:b.templates)if(!Same(id))return false;
    for(const auto& part:b.parts)if(part.created){
        if(!ue::Alive(part.created)||ue::ClassOf(part.created)!=ue::ClassOf(part.source))return false;
        for(const char* asset:{"SkinnedAsset","StaticMesh","GroomAsset","BindingAsset"})if(Property<Object*>(part.created,asset)!=Property<Object*>(part.source,asset))return false;
    }
    return true;
}
const char* pool_step_="idle";
bool PoolFreeze(DisplayBundle& b){
    pool_step_="retire bone supplement";
    for(const auto& id:b.owned)if(Same(id)&&ue::IsA(id.object,Class("/Script/Engine.SkeletalMeshComponent"))&&!BoneMotionRuntime::BeforeDormancy(id.object,FaceQuiesce))return false;
    std::vector<Identity> objects=b.owned;
    for(const auto& id:b.owned){
        if(!Same(id))return false;
        pool_step_="collect children";Call descendants(id.object,"GetChildrenComponents");
        if(!descendants.Set("bIncludeAllDescendants",true).Invoke())return false;
        const Array children=descendants.Get<Array>("Children");
        if(children.num<0||children.num>1024||children.max<children.num||(children.num&&!children.data)){ue::Free(children.data);return false;}
        bool good=true;
        for(int i=0;i<children.num;++i){Object* child=static_cast<Object**>(children.data)[i];if(!ue::Alive(child))continue;
            Call owner(child,"GetOwner");if(!owner.Invoke()||owner.Get<Object*>("ReturnValue")!=b.pawn.object){
                Logf("NPC_POOL refused id=%s reason=foreign-child component=%s owner=%s parent=%s",kProfiles[b.row].id,Text(child).c_str(),Text(owner.Get<Object*>("ReturnValue")).c_str(),Text(Property<Object*>(child,"AttachParent")).c_str());good=false;break;}
            if(std::none_of(objects.begin(),objects.end(),[&](const Identity& item){return item.object==child;}))objects.push_back(Id(child));
        }
        ue::Free(children.data);if(!good)return false;
    }
    for(const auto& id:objects){
        Object* component=id.object;
        if(ue::IsA(component,Class("/Script/Engine.SkeletalMeshComponent"))){if(!FaceQuiesce(component))return false;BoneMotionRuntime::BeforeComponentHook(component);}
        auto found=std::find_if(b.dormant.begin(),b.dormant.end(),[&](const DormantState& saved){return saved.component.object==component;});
        if(found==b.dormant.end()){
            pool_step_="read dormant state";DormantState saved;saved.component=id;Call tick(component,"IsComponentTickEnabled");
            if(!tick.Invoke()||!VisibilityOf(component,saved.visible,saved.hidden))return false;saved.tick=tick.Get<bool>("ReturnValue");
            saved.has_shadow=Bool(component,"CastShadow",saved.shadow)&&Bool(component,"bCastHiddenShadow",saved.hidden_shadow);
            saved.primitive=ue::IsA(component,Class("/Script/Engine.PrimitiveComponent"));
            if(saved.primitive){Call collision(component,"GetCollisionEnabled"),overlap(component,"GetGenerateOverlapEvents");
                if(!collision.Invoke()||!overlap.Invoke())return false;saved.collision=collision.Get<uint8_t>("ReturnValue");saved.overlap=overlap.Get<bool>("ReturnValue");
                Call simulation(component,"IsSimulatingPhysics");if(!simulation.Set("BoneName",Name{}).Invoke()||simulation.Get<bool>("ReturnValue"))return false;}

            saved.fx=ue::IsA(component,Class("/Script/Engine.FXSystemComponent"));
            if(saved.fx){Call active(component,"IsActive");if(!active.Invoke())return false;saved.active=active.Get<bool>("ReturnValue");}
            saved.cloth=ue::IsA(component,Class("/Script/ChaosClothAssetEngine.ChaosClothComponent"));
            if(saved.cloth){Call simulation(component,"IsSimulationEnabled");if(!simulation.Invoke())return false;saved.simulation=simulation.Get<bool>("ReturnValue");}
            b.dormant.push_back(saved);found=b.dormant.end()-1;
        }
        pool_step_="hide and stop tick";if(!SetVisible(component,false,true)||!Call(component,"SetComponentTickEnabled").Set("bEnabled",false).Invoke())return false;
        pool_step_="shadow and collision";if(found->has_shadow&&(!PutBool(component,"bCastHiddenShadow",false)||!Call(component,"SetCastShadow").Set("NewCastShadow",false).Invoke()))return false;
        if(ue::IsA(component,Class("/Script/Engine.PrimitiveComponent"))&&(!Call(component,"SetCollisionEnabled").Set("NewType",uint8_t{0}).Invoke()||!Call(component,"SetGenerateOverlapEvents").Set("bInGenerateOverlapEvents",false).Invoke()))return false;
        pool_step_="effect and cloth simulation";if(found->fx&&found->active&&!Call(component,"Deactivate").Invoke())return false;
        if(found->cloth&&!Call(component,"SetEnableSimulation").Set("bEnable",false).Invoke())return false;
        pool_step_="verify dormancy";Call tick(component,"IsComponentTickEnabled");bool visible=false,hidden=false;
        if(!tick.Invoke()||tick.Get<bool>("ReturnValue")||!VisibilityOf(component,visible,hidden)||visible||!hidden)return false;
    }
    return true;
}
bool PoolWake(DisplayBundle& b){
    if(!PoolValid(b))return false;
    for(const auto& saved:b.dormant){
        if(!Same(saved.component))continue;Object* component=saved.component.object;
        if(saved.has_shadow&&(!PutBool(component,"bCastHiddenShadow",saved.hidden_shadow)||!Call(component,"SetCastShadow").Set("NewCastShadow",saved.shadow).Invoke()))return false;
        const bool own=std::any_of(b.owned.begin(),b.owned.end(),[&](const Identity& id){return id.object==component;});
        if(saved.primitive&&(!Call(component,"SetCollisionEnabled").Set("NewType",saved.collision).Invoke()||!Call(component,"SetGenerateOverlapEvents").Set("bInGenerateOverlapEvents",saved.overlap).Invoke()))return false;
        if(saved.cloth&&(!Call(component,"SetEnableSimulation").Set("bEnable",own?true:saved.simulation).Invoke()||!Call(component,"ForceNextUpdateTeleportAndReset").Invoke()))return false;
        if(saved.fx&&saved.active&&!Call(component,"Activate").Set("bReset",true).Invoke())return false;
        if(!Call(component,"SetComponentTickEnabled").Set("bEnabled",saved.tick).Invoke())return false;
        // Body/parts use fresh authored visibility, including the face-ready gate.
        // Only engine-created auxiliary children restore their captured flags.
        if(!own&&!SetVisible(component,saved.visible,saved.hidden))return false;
    }
    b.dormant.clear();b.used=GetTickCount64();return true;
}
bool PoolDrop(DisplayBundle& b){
    b.broken=true;
    for(size_t i=b.owned.size();i-->0;){
        const auto id=b.owned[i];
        if(Same(id)&&ue::IsA(id.object,Class("/Script/Engine.SkeletalMeshComponent"))){if(!FaceQuiesce(id.object))continue;BoneMotionRuntime::BeforeComponentHook(id.object);}
        if(!Same(id)||Destroy(id.object))b.owned.erase(b.owned.begin()+static_cast<std::ptrdiff_t>(i));
    }
    return b.owned.empty();
}
// Parent visibility and native hair-mode changes may touch attached components.
// Keep dormant models hidden; expensive state queries run only every 250 ms.
void HoldPoolDormancy(){
    static ULONGLONG next=0;const ULONGLONG now=GetTickCount64();const bool deep=now>=next;if(deep)next=now+250;
    for(auto& b:display_pool_.Items())for(const auto& saved:b->dormant){
        if(!Same(saved.component))continue;Object* component=saved.component.object;bool visible=false,hidden=false;
        if(VisibilityOf(component,visible,hidden)&&(visible||!hidden))SetVisible(component,false,true);
        if(!deep)continue;
        Call tick(component,"IsComponentTickEnabled");if(tick.Invoke()&&tick.Get<bool>("ReturnValue"))Call(component,"SetComponentTickEnabled").Set("bEnabled",false).Invoke();
        if(saved.has_shadow){bool shadow=false,hidden_shadow=false;if(Bool(component,"bCastHiddenShadow",hidden_shadow)&&hidden_shadow)PutBool(component,"bCastHiddenShadow",false);
            if(Bool(component,"CastShadow",shadow)&&shadow)Call(component,"SetCastShadow").Set("NewCastShadow",false).Invoke();}
        if(saved.primitive){Call collision(component,"GetCollisionEnabled"),overlap(component,"GetGenerateOverlapEvents");
            if(collision.Invoke()&&collision.Get<uint8_t>("ReturnValue")!=0)Call(component,"SetCollisionEnabled").Set("NewType",uint8_t{0}).Invoke();
            if(overlap.Invoke()&&overlap.Get<bool>("ReturnValue"))Call(component,"SetGenerateOverlapEvents").Set("bInGenerateOverlapEvents",false).Invoke();}
        if(saved.fx){Call active(component,"IsActive");if(active.Invoke()&&active.Get<bool>("ReturnValue"))Call(component,"Deactivate").Invoke();}
        if(saved.cloth){Call enabled(component,"IsSimulationEnabled");if(enabled.Invoke()&&enabled.Get<bool>("ReturnValue"))Call(component,"SetEnableSimulation").Set("bEnable",false).Invoke();}
    }
}
bool PoolClear(){return display_pool_.Clear(PoolDrop);}
bool PoolParkActive(){
    if(!pool_enabled_||!InterlockedCompareExchange(&experimental_,0,0)||!active_bundle_||cutscene_host_||!source_driver_mode_)return false;
    if(!PoolValid(*active_bundle_)||added_.size()!=active_bundle_->owned.size()){
        Logf("NPC_POOL refused id=%s reason=identity-or-owned-set owned=%zu active=%zu",kProfiles[active_bundle_->row].id,active_bundle_->owned.size(),added_.size());return false;}
    Context current;if(!ContextNow(current)||current.pawn!=active_bundle_->pawn.object||current.mesh!=active_bundle_->source.object||current.world!=active_bundle_->world.object){
        Logf("NPC_POOL refused id=%s reason=context-changed",kProfiles[active_bundle_->row].id);return false;}
    if(!display_pool_.MakeRoom(PoolDrop)){Logf("NPC_POOL refused id=%s reason=eviction-failed",kProfiles[active_bundle_->row].id);return false;}
    if(!PoolFreeze(*active_bundle_)){Logf("NPC_POOL refused id=%s reason=freeze stage=%s",kProfiles[active_bundle_->row].id,pool_step_);return false;}
    active_bundle_->ready=true;active_bundle_->used=GetTickCount64();
    Logf("NPC_POOL parked id=%s components=%zu dormant=%zu",kProfiles[active_bundle_->row].id,active_bundle_->owned.size(),active_bundle_->dormant.size());
    return display_pool_.Put(active_bundle_);
}
std::unique_ptr<DisplayBundle> PoolTake(const Context& context,int row,Object* donor,Object* target,std::vector<Part>& parts){
    if(!pool_enabled_)return {};
    display_pool_.Prune([&](const DisplayBundle& b){return PoolValid(b)&&(b.row!=row||b.ready);},PoolDrop);
    auto bundle=display_pool_.Take([&](const DisplayBundle& b){
        if(!b.ready||b.row!=row||b.pawn.object!=context.pawn||b.source.object!=context.mesh||b.world.object!=context.world||b.donor.object!=donor||b.target.object!=target||b.parts.size()!=parts.size())return false;
        for(size_t i=0;i<parts.size();++i)if(b.parts[i].source!=parts[i].source||b.parts[i].variable!=parts[i].variable||b.parts[i].parent!=parts[i].parent||!(b.parts[i].socket==parts[i].socket))return false;
        return true;
    });
    if(bundle)for(size_t i=0;i<parts.size();++i){parts[i].created=bundle->parts[i].created;parts[i].finished=false;}
    return bundle;
}
void WarmDisplayPool(const Context& context,const std::vector<int32_t>& wanted){
    if(!pool_enabled_||!NativeHelpersMatch()||!AnimationApiReady(context.mesh))return;
    const char* previous_stage=stage_;struct StageRestore{const char* previous;~StageRestore(){stage_=previous;}} restore_stage{previous_stage};stage_="warm hidden NPC display";
    DisplayBundle* work=nullptr;
    for(auto& b:display_pool_.Items())if(!b->broken&&!b->ready){work=b.get();break;}
    if(!work){
        if(display_pool_.Full())return;
        int row=-1;
        for(int candidate:wanted){if(!PoolEligible(candidate)||pool_attempts_[candidate]>=2||(active_&&selected_==candidate))continue;
            if(std::none_of(display_pool_.Items().begin(),display_pool_.Items().end(),[&](const auto& b){return b->row==candidate;})){row=candidate;break;}}
        if(row<0)return;
        const Profile& profile=kProfiles[row];const std::string path=profile.donor;
        Object* cls=Class((path+"."+path.substr(path.find_last_of('/')+1)+"_C").c_str());
        Object* copy=Class("/HuaShira/Characters/Hero/Hero_Hun/Hun_CopyPose.Hun_CopyPose_C");
        if(!ue::Alive(cls)||!ue::Alive(copy))return; // no blocking loader on this path
        Object* donor=Property<Object*>(DefaultObject(cls),"Mesh"),*target=SkelMesh(donor);
        if(!ue::Alive(target)||target!=Class(profile.body))return;
        auto next=std::make_unique<DisplayBundle>();next->row=row;next->pawn=Id(context.pawn);next->source=Id(context.mesh);next->source_asset=Id(SkelMesh(context.mesh));next->world=Id(context.world);
        next->donor=Id(donor);next->target=Id(target);next->copy_class=Id(copy);next->used=GetTickCount64();
        AnimOptions options;if(!ReadAnimOptions(donor,options)||!LoadParts(cls,DefaultObject(cls),profile,next->parts)||!ValidateSourceFace(profile,next->parts))return;
        for(const auto& part:next->parts)next->templates.push_back(Id(part.source));
        if(options.disabled&&!options.override_class&&Property<Object*>(target,"PostProcessAnimBlueprint"))options.disabled=false;
        ++pool_attempts_[row];
        if(!display_pool_.Put(next))return;work=display_pool_.Items().back().get();
        const LONGLONG started=QpcNow();Object* body=Add(context.pawn,Class("/Script/Engine.SkeletalMeshComponent"));
        if(body){work->body=Id(body);work->owned.push_back(work->body);}
        const bool ok=body&&SetVisible(body,false,true)&&SetMesh(body,target)&&WriteAnimOptions(body,options)&&CopyMaterials(donor,body)&&
            SetPhysics(body,Property<Object*>(donor,"PhysicsAssetOverride"))&&Call(body,"SetAnimInstanceClass").Set("NewClass",copy).Invoke()&&
            Put(body,"VisibilityBasedAnimTickOption",uint8_t{0})&&PutBool(body,"bEnableUpdateRateOptimizations",false)&&
            Call(body,"SetCollisionEnabled").Set("NewType",uint8_t{0}).Invoke()&&Call(body,"SetGenerateOverlapEvents").Set("bInGenerateOverlapEvents",false).Invoke()&&
            Call(body,"AddTickPrerequisiteComponent").Set("PrerequisiteComponent",context.mesh).Invoke()&&AttachFinish(context.pawn,body,context.mesh,Name{},Transform{});
        if(ok){CopyRenderFlags(donor,body);work->broken=!MatchPlayerLighting(context.mesh,body)||!PoolFreeze(*work);}else work->broken=true;
        Logf("NPC_POOL warm body id=%s ok=%d stage=%s ms=%.3f",profile.id,work->broken?0:1,pool_step_,QpcMs(started));return;
    }
    if(!PoolValid(*work)){work->broken=true;Logf("NPC_POOL warm invalidated id=%s",kProfiles[work->row].id);return;}
    Context display_context=context;display_context.mesh=work->body.object;
    const LONGLONG started=QpcNow();
    if(!Assemble(display_context,work->donor.object,kProfiles[work->row],work->parts,work)){work->broken=true;Logf("NPC_POOL warm failed id=%s stage=%s; cold assembly retained",kProfiles[work->row].id,pool_step_);return;}
    work->ready=std::all_of(work->parts.begin(),work->parts.end(),[](const Part& part){return part.finished;});
    Logf("NPC_POOL warm step id=%s ready=%d components=%zu ms=%.3f",kProfiles[work->row].id,work->ready?1:0,work->owned.size(),QpcMs(started));
}
