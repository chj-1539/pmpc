//=============================================================================
// redundancy.cxx - dual machine redundancy implementation
//=============================================================================
#include "redundancy.h"
#include "str_util.h"
#include <iostream>
#include <fstream>
#include <cstring>
#include <chrono>
#include <algorithm>

std::string ChangeEvent::ToString() const {
    static const char*TYPES[]={"?","DI","AI","DO_M","DO_S","AO"};
    const char*tn=(type>=1&&type<=5)?TYPES[type]:"?";
    char b[128];
    if(type==AI_CHANGE||type==AO_CHANGE)
        std::snprintf(b,sizeof(b),"%s ch=%u dev=%u pt=%u val=%.2f",tn,channel,device,point,dvalue);
    else
        std::snprintf(b,sizeof(b),"%s ch=%u dev=%u pt=%u val=%llu",tn,channel,device,point,(unsigned long long)value);
    return b;
}

std::vector<uint8_t>SyncChannel::BuildSyncFrame(const ChangeEvent& ev){
    std::vector<uint8_t> f;
    f.push_back(SYNC_START);f.push_back(SYNC_FUN);
    f.push_back(0);f.push_back(0);
    f.push_back(ev.type);
    f.push_back((uint8_t)(ev.channel&0xFF));f.push_back((uint8_t)((ev.channel>>8)&0xFF));
    f.push_back((uint8_t)(ev.device&0xFF));f.push_back((uint8_t)((ev.device>>8)&0xFF));
    f.push_back((uint8_t)(ev.point&0xFF));f.push_back((uint8_t)((ev.point>>8)&0xFF));
    uint64_t rv;memcpy(&rv,&ev.dvalue,sizeof(rv));
    for(int i=0;i<8;i++)f.push_back((uint8_t)((rv>>(i*8))&0xFF));
    for(int i=0;i<8;i++)f.push_back((uint8_t)((ev.tsMs>>(i*8))&0xFF));
    f[2]=23;f[3]=0;f.push_back(SYNC_END);
    return f;
}
bool SyncChannel::ParseSyncFrame(const uint8_t*d,size_t len,ChangeEvent& ev){
    if(len<28||d[0]!=SYNC_START||d[1]!=SYNC_FUN||d[len-1]!=SYNC_END)return false;
    unsigned a,b;
    a=(unsigned)d[5];b=(unsigned)d[6]<<8;ev.channel=(uint16_t)(a|b);
    a=(unsigned)d[7];b=(unsigned)d[8]<<8;ev.device=(uint16_t)(a|b);
    a=(unsigned)d[9];b=(unsigned)d[10]<<8;ev.point=(uint16_t)(a|b);
    ev.type=d[4];
    uint64_t rv=0;
    for(int i=0;i<8;i++)rv|=(uint64_t)d[11+i]<<(i*8);
    memcpy(&ev.dvalue,&rv,sizeof(ev.dvalue));ev.value=(uint64_t)ev.dvalue;
    ev.tsMs=0;
    for(int i=0;i<8;i++)ev.tsMs|=(uint64_t)d[19+i]<<(i*8);
    return true;
}
bool SyncChannel::StartMaster(uint16_t port){
    port_=port;running_=true;
    try{listenSock_.bind(socket_addr("0.0.0.0",port_,protocol_type::tcp));listenSock_.listen(1);
        std::cout<<"[Sync] Master listen "<<port_<<std::endl;}
    catch(const std::exception&e){std::cerr<<"[Sync] bind fail: "<<e.what()<<std::endl;running_=false;return false;}
    acceptThr_=std::thread(&SyncChannel::AcceptLoop,this);
    return true;
}
bool SyncChannel::StartStandby(const std::string&host,uint16_t port){
    peerHost_=host;port_=port;running_=true;
    std::cout<<"[Sync] Standby connect "<<host<<":"<<port<<std::endl;
    recvThr_=std::thread([this](){RecvLoop(socket(),socket_addr());});return true;
}
void SyncChannel::Stop(){
    running_=false;
    try{listenSock_.close();}catch(...){}
    {std::lock_guard<std::mutex>lk(clientMtx_);try{clientSock_.close();}catch(...){}}
    try{syncSock_.close();}catch(...){}
    if(acceptThr_.joinable())acceptThr_.join();
    if(recvThr_.joinable())recvThr_.join();
}
void SyncChannel::AcceptLoop(){
    while(running_){
        try{socket_addr p;socket c=listenSock_.accept(&p);
            std::cout<<"[Sync] Standby connected: "<<p.to_string()<<std::endl;
            {std::lock_guard<std::mutex>lk(clientMtx_);clientSock_=std::move(c);}}
        catch(const socket_error&e){if(running_)std::cerr<<"[Sync] accept: "<<e.what()<<std::endl;}
    }
}
void SyncChannel::RecvLoop(socket s,socket_addr){
    uint8_t buf[4096];
    while(running_){
        if(!s.is_open()){
            try{s.connect(peerHost_,port_);s.set_recv_timeout(std::chrono::milliseconds(500));
                std::cout<<"[Sync] Connected to Master"<<std::endl;}
            catch(...){if(running_)std::this_thread::sleep_for(std::chrono::milliseconds(500));continue;}
        }
        try{
            size_t n=s.recv(buf,2);
            if(n!=2||buf[0]!=SYNC_START||buf[1]!=SYNC_FUN)continue;
            n=s.recv(buf+2,2);if(n!=2)continue;
            unsigned dl=(unsigned)buf[2]|((unsigned)buf[3]<<8);
            uint16_t dl16=(uint16_t)dl;size_t tl=(size_t)dl16+5;
            if(tl>sizeof(buf))continue;
            size_t r=tl-4,t=0;while(t<r){n=s.recv(buf+4+t,r-t);if(n==0)break;t+=n;}
            ChangeEvent ev;
            if(ParseSyncFrame(buf,tl,ev)&&callback_)callback_(ev);
        }catch(const socket_error&e){
            std::cerr<<"[Sync] disconnect: "<<e.what()<<std::endl;
            try{s.close();}catch(...){}
        }
    }
}
bool SyncChannel::SendChange(const ChangeEvent& ev){
    auto f=BuildSyncFrame(ev);
    std::lock_guard<std::mutex>lk(clientMtx_);
    if(!clientSock_.is_open())return false;
    size_t t=0;while(t<f.size())t+=clientSock_.send(f.data()+t,f.size()-t);
    return true;
}

RedundancyManager::RedundancyManager(){}
RedundancyManager::~RedundancyManager(){Stop();}

bool RedundancyManager::LoadConfig(const std::string& cp){
    std::ifstream fi(cp);
    if(!fi.is_open()){std::cerr<<"[Redundancy] cannot open: "<<cp<<std::endl;return true;}
    std::string l;
    while(std::getline(fi,l)){
        if(!l.empty()&&l.back()=='\r')l.pop_back();
        l=Trim(l);if(l.empty()||l[0]==';'||l[0]=='#')continue;
        if(l.front()=='[')continue;
        size_t e=l.find('=');
        if(e==std::string::npos)continue;
        ParseConfigLine(Trim(l.substr(0,e)),Trim(l.substr(e+1)));
    }
    fi.close();
    std::cout<<"[Redundancy] cfg local="<<localName_<<" peer="<<peerIp_
             <<" hb="<<heartbeatPort_<<" pri="<<priority_<<std::endl;
    return true;
}
bool RedundancyManager::ParseConfigLine(const std::string&k,const std::string&v){
    std::string l=k;for(char&c:l)if(c>='A'&&c<='Z')c=(char)(c+32);
    if(l=="local_name")localName_=v;
    else if(l=="peer_ip")peerIp_=v;
    else if(l=="heartbeat_port")heartbeatPort_=static_cast<uint16_t>(SafeStoi(v));
    else if(l=="sync_port")syncPort_=static_cast<uint16_t>(SafeStoi(v));
    else if(l=="heartbeat_interval_ms")heartbeatIntervalMs_=SafeStoi(v);
    else if(l=="missed_heartbeat_limit")missedHeartbeatLimit_=SafeStoi(v);
    else if(l=="auto_failback")autoFailback_=(v=="1"||v=="true");
    else if(l=="startup_delay_ms")startupDelayMs_=SafeStoi(v);
    else if(l=="priority")priority_=SafeStoi(v);
    else return false;
    return true;
}
bool RedundancyManager::Start(){
    if(running_)return false;
    running_=true;role_=RedundRole::Idle;peerAlive_=false;
    missedHeartbeats_=0;startupCompleteTime_=NowMs()+startupDelayMs_;
    std::cout<<"\n===== Redundancy Start =====\n"
             <<" local="<<localName_<<" pri="<<priority_
             <<"\n peer="<<peerIp_<<":"<<heartbeatPort_
             <<"\n"<<std::endl;
    try{hbListenSock_.bind(socket_addr("0.0.0.0",heartbeatPort_,protocol_type::tcp));hbListenSock_.listen(1);
        hbListenThr_=std::thread(&RedundancyManager::ListenLoop,this);}
    catch(const std::exception&e){std::cerr<<"[Redundancy] hb bind: "<<e.what()<<std::endl;}
    hbSendThr_=std::thread(&RedundancyManager::HeartbeatLoop,this);
    std::cout<<"[Redundancy] started, wait "<<(startupDelayMs_/1000)<<"s for role decision"<<std::endl;
    return true;
}
void RedundancyManager::Stop(){
    running_=false;
    try{hbListenSock_.close();}catch(...){}
    { std::lock_guard<std::mutex> lock(hbMtx_); try{hbSendSock_.close();}catch(...){} }
    syncChannel_.Stop();
    if(hbSendThr_.joinable())hbSendThr_.join();
    if(hbListenThr_.joinable())hbListenThr_.join();
}
void RedundancyManager::HeartbeatLoop(){
    while(running_){
        if(NowMs()<startupCompleteTime_){std::this_thread::sleep_for(std::chrono::milliseconds(200));continue;}
        {
            std::lock_guard<std::mutex> lock(hbMtx_);
            if(!running_) break;
            if(!hbSendSock_.is_open()){
                try{hbSendSock_.connect(peerIp_,heartbeatPort_);hbSendSock_.set_recv_timeout(std::chrono::milliseconds(500));
                    std::cout<<"[Redundancy] hb connected -> "<<peerIp_<<":"<<heartbeatPort_<<std::endl;}
                catch(...){/* 连接失败，由外层循环定时重试 */ try{hbSendSock_.close();}catch(...){} }
            }
            if(hbSendSock_.is_open()) try{
                uint8_t f[14];f[0]=HB_START;f[1]=HB_FUN;f[2]=8;f[3]=0;
                f[4]=(uint8_t)role_.load();uint64_t ts=NowMs();
                for(int i=0;i<8;i++)f[5+i]=(uint8_t)((ts>>(i*8))&0xFF);
                f[13]=HB_END;
                size_t t=0;while(t<14)t+=hbSendSock_.send(f+t,14-t);
            }catch(const socket_error&e){
                std::cerr<<"[Redundancy] hb send: "<<e.what()<<std::endl;
                try{hbSendSock_.close();}catch(...){}peerAlive_=false;CheckFailover();
            }
        }
        for(int i=0;i<heartbeatIntervalMs_/100&&running_;i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}
void RedundancyManager::ListenLoop(){
    while(running_){
        try{socket_addr p;socket c=hbListenSock_.accept(&p);
            std::cout<<"[Redundancy] hb from: "<<p.to_string()<<std::endl;
            while(running_){
                uint8_t b[14];size_t t=0;
                while(t<14){size_t n=c.recv(b+t,14-t);if(n==0)break;t+=n;}
                if(t<14)break;
                if(b[0]==HB_START&&b[1]==HB_FUN&&b[13]==HB_END){
                    missedHeartbeats_=0;
                    peerRole_ = static_cast<RedundRole>(b[4]);
                    if(!peerAlive_){peerAlive_=true;std::cout<<"[Redundancy] peer online"<<std::endl;}
                    CheckFailover();
                }
            }
            c.close();
        }catch(const socket_error&){if(running_)std::this_thread::sleep_for(std::chrono::milliseconds(200));}
    }
}
void RedundancyManager::CheckFailover(){
    if(!running_||NowMs()<startupCompleteTime_)return;
    if(!peerAlive_){
        missedHeartbeats_++;
        if(missedHeartbeats_>=missedHeartbeatLimit_){
            if(role_==RedundRole::Standby){
                // 备机超时 → 升主
                SetRole(RedundRole::Master);
            }else if(role_==RedundRole::Idle){
                // Idle 超时 → 升主（启动时对端未上线）
                SetRole(RedundRole::Master);
            }
        }
    }else{
        // 对端在线时的角色决策
        if(role_==RedundRole::Idle){
            // 对端已是 Master → 本站为 Standby
            // 对端是 Standby/Idle → 本站根据优先级决策
            if(peerRole_==RedundRole::Master){
                SetRole(RedundRole::Standby);
            }else{
                // 两个 Idle 或两个 Standby：优先级高的升主
                // 若优先级相同，优先留在 Standby 避免双主
                SetRole(priority_ > peerPriority_ ? RedundRole::Master : RedundRole::Standby);
            }
        }else if(role_==RedundRole::Master && peerRole_==RedundRole::Master){
            // 双主检测：降级优先级低的
            if(priority_ < peerPriority_){
                std::cerr<<"[Redundancy] 双主检测，本站降级"<<std::endl;
                SetRole(RedundRole::Standby);
            }
        }
    }
}
void RedundancyManager::SetRole(RedundRole nr){
    if(role_==nr)return;
    RedundRole or_=role_;role_=nr;
    std::cout<<"\n*** "<<RoleName(or_)<<" -> "<<RoleName(nr)<<" ***\n"<<std::endl;
    if(nr==RedundRole::Master){syncChannel_.StartMaster(syncPort_);syncChannel_.SetChangeCallback(nullptr);}
    else if(nr==RedundRole::Standby){syncChannel_.StartStandby(peerIp_,syncPort_);
        syncChannel_.SetChangeCallback([this](const ChangeEvent&ev){OnSyncData((ChangeEvent::Type)ev.type,ev.channel,ev.device,ev.point,ev.value,ev.dvalue);});}
    else syncChannel_.Stop();
    if(collectCb_)collectCb_(nr==RedundRole::Master);
    if(roleCb_)roleCb_(or_,nr);
}
void RedundancyManager::RequestRoleChange(RedundRole newRole){
    std::cout<<"[Redundancy] 调试控制台请求角色切换: "<<RoleName(newRole)<<std::endl;
    SetRole(newRole);
}
void RedundancyManager::OnSyncData(ChangeEvent::Type t,uint16_t ch,uint16_t dev,uint16_t pt,uint64_t val,double dval){
    if(t==ChangeEvent::DO_MASTER_CHANGE||t==ChangeEvent::DO_SLAVE_CHANGE)return;
    auto&m=RemoteDataMgr::Instance();uint64_t n=NowMs();
    switch(t){
    case ChangeEvent::DI_CHANGE:m.SetDi(ch,dev,pt,val!=0,n,true);break;
    case ChangeEvent::AI_CHANGE:m.SetAi(ch,dev,pt,dval);break;
    case ChangeEvent::AO_CHANGE:m.SetAo(ch,dev,pt,dval);break;
    default:break;
    }
}
