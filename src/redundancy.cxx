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

// ============================================================================
// pmpc::redundancy 纯编解码 + 角色决策
// ============================================================================
namespace pmpc { namespace redundancy {

std::vector<uint8_t> BuildHeartbeatFrame(RedundRole role, uint16_t priority, uint64_t tsMs)
{
    // 新格式：payload = ROLE(1) + TS(8) + PRI(2) = 11 字节，LEN 存 payload 长度。
    // 总帧长 = 1(START) + 1(FUN) + 2(LEN) + 11(payload) + 1(END) = 16 字节。
    // 注意：老实现的心跳帧 LEN 字段写死为 8（未含 ROLE，且从未被 ListenLoop
    // 校验，因为老代码定长 recv 14 字节）。为向前兼容，ParseHeartbeatFrame
    // 同时接受 LEN=8（老帧长 14）和 LEN=11（新帧长 16）。
    std::vector<uint8_t> f;
    f.reserve(16);
    f.push_back(HB_START);
    f.push_back(HB_FUN);
    f.push_back(11);              // LEN low = payload 长度
    f.push_back(0);               // LEN high
    f.push_back(static_cast<uint8_t>(role));
    for (int i = 0; i < 8; i++) f.push_back(static_cast<uint8_t>((tsMs >> (i * 8)) & 0xFF));
    f.push_back(static_cast<uint8_t>(priority & 0xFF));
    f.push_back(static_cast<uint8_t>((priority >> 8) & 0xFF));
    f.push_back(HB_END);
    return f;
}

bool ParseHeartbeatFrame(const uint8_t* data, size_t len, HeartbeatInfo& out)
{
    if (len < 14 || data[0] != HB_START || data[1] != HB_FUN) return false;
    const unsigned dl = static_cast<unsigned>(data[2]) |
                        (static_cast<unsigned>(data[3]) << 8);
    // 兼容层：
    //   老帧：LEN=8，总长 14（LEN 字段实际只覆盖 TS 8 字节，未含 ROLE）
    //   新帧：LEN=11，总长 16（LEN 覆盖 ROLE+TS+PRI 全 payload）
    size_t expected = 0;
    if      (dl == 8)  expected = 14;
    else if (dl == 11) expected = 16;
    else               return false;
    if (len < expected) return false;
    if (data[expected - 1] != HB_END) return false;

    out.role = static_cast<RedundRole>(data[4]);
    out.tsMs = 0;
    for (int i = 0; i < 8; i++)
        out.tsMs |= static_cast<uint64_t>(data[5 + i]) << (i * 8);
    if (expected == 16) {
        out.priority = static_cast<uint16_t>(
            static_cast<unsigned>(data[13]) |
            (static_cast<unsigned>(data[14]) << 8));
    } else {
        out.priority = 0;   // 老格式：无 priority 信息
    }
    return true;
}

std::vector<uint8_t> BuildSyncFrame(const ChangeEvent& ev)
{
    std::vector<uint8_t> f;
    f.reserve(28);
    f.push_back(SYNC_START);
    f.push_back(SYNC_FUN);
    f.push_back(23);      // LEN low  = 23 (payload 长度)
    f.push_back(0);       // LEN high
    f.push_back(ev.type);
    f.push_back(static_cast<uint8_t>(ev.channel & 0xFF));
    f.push_back(static_cast<uint8_t>((ev.channel >> 8) & 0xFF));
    f.push_back(static_cast<uint8_t>(ev.device  & 0xFF));
    f.push_back(static_cast<uint8_t>((ev.device  >> 8) & 0xFF));
    f.push_back(static_cast<uint8_t>(ev.point   & 0xFF));
    f.push_back(static_cast<uint8_t>((ev.point   >> 8) & 0xFF));
    uint64_t rv;
    std::memcpy(&rv, &ev.dvalue, sizeof(rv));
    for (int i = 0; i < 8; i++) f.push_back(static_cast<uint8_t>((rv >> (i * 8)) & 0xFF));
    for (int i = 0; i < 8; i++) f.push_back(static_cast<uint8_t>((ev.tsMs >> (i * 8)) & 0xFF));
    f.push_back(SYNC_END);
    return f;
}

bool ParseSyncFrame(const uint8_t* d, size_t len, ChangeEvent& ev)
{
    if (len < 28 || d[0] != SYNC_START || d[1] != SYNC_FUN || d[len - 1] != SYNC_END)
        return false;
    unsigned a, b;
    a = static_cast<unsigned>(d[5]);  b = static_cast<unsigned>(d[6])  << 8; ev.channel = static_cast<uint16_t>(a | b);
    a = static_cast<unsigned>(d[7]);  b = static_cast<unsigned>(d[8])  << 8; ev.device  = static_cast<uint16_t>(a | b);
    a = static_cast<unsigned>(d[9]);  b = static_cast<unsigned>(d[10]) << 8; ev.point   = static_cast<uint16_t>(a | b);
    ev.type = d[4];
    uint64_t rv = 0;
    for (int i = 0; i < 8; i++) rv |= static_cast<uint64_t>(d[11 + i]) << (i * 8);
    std::memcpy(&ev.dvalue, &rv, sizeof(ev.dvalue));
    ev.value = static_cast<uint64_t>(ev.dvalue);
    ev.tsMs  = 0;
    for (int i = 0; i < 8; i++) ev.tsMs |= static_cast<uint64_t>(d[19 + i]) << (i * 8);
    return true;
}

RedundRole DecideRole(RedundRole role, RedundRole peerRole, bool peerAlive,
                      int missedHeartbeats, int missedLimit,
                      uint16_t localPriority, uint16_t peerPriority)
{
    if (!peerAlive) {
        if (missedHeartbeats >= missedLimit &&
            (role == RedundRole::Standby || role == RedundRole::Idle)) {
            return RedundRole::Master;
        }
        return role;
    }
    // peerAlive
    if (role == RedundRole::Idle) {
        if (peerRole == RedundRole::Master) return RedundRole::Standby;
        // 两个 Idle 或 Idle+Standby：优先级高升主，等优先级留 Standby 避免双主
        return (localPriority > peerPriority) ? RedundRole::Master : RedundRole::Standby;
    }
    if (role == RedundRole::Master && peerRole == RedundRole::Master) {
        // 双主检测：本地优先级严格低才降级；等优先级维持 Master 由通信另一端降
        if (localPriority < peerPriority) return RedundRole::Standby;
    }
    return role;
}

}} // namespace pmpc::redundancy

std::vector<uint8_t>SyncChannel::BuildSyncFrame(const ChangeEvent& ev){
    return pmpc::redundancy::BuildSyncFrame(ev);
}
bool SyncChannel::ParseSyncFrame(const uint8_t*d,size_t len,ChangeEvent& ev){
    return pmpc::redundancy::ParseSyncFrame(d, len, ev);
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
    // L5 修复：以前打不开配置就 return true，让 Start() 用硬编码默认值
    // (peerIp_=127.0.0.1)。两台机器都没配置就会互相认为对方在 localhost，
    // 双主同机撞车。改为 return false —— ModuleManager 会跳过 redundancy
    // 模块，比"默认双主"安全得多。
    if(!fi.is_open()){
        std::cerr<<"[Redundancy] 无法打开配置文件: "<<cp
                 <<" — 已禁用冗余模块（改回 return true 会以 localhost 默认启动，"
                 <<"极易造成同机双主）"<<std::endl;
        return false;
    }
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
                // 修复 C4：心跳里携带本机 priority，对端 ListenLoop 可解析并
                // 存入 peerPriority_，供 CheckFailover 双主/等优先决策。
                auto f = pmpc::redundancy::BuildHeartbeatFrame(
                    role_.load(), static_cast<uint16_t>(priority_), NowMs());
                size_t t=0;while(t<f.size())t+=hbSendSock_.send(f.data()+t,f.size()-t);
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
                // 先读 START+FUN+LEN(2)，再依 LEN 读剩余；兼容老 14 字节 / 新 16 字节
                uint8_t hdr[4]; size_t t=0;
                while(t<4){size_t n=c.recv(hdr+t,4-t);if(n==0)break;t+=n;}
                if(t<4)break;
                unsigned dl = static_cast<unsigned>(hdr[2]) |
                              (static_cast<unsigned>(hdr[3]) << 8);
                if (dl != 8 && dl != 11) { continue; }  // 无效帧
                size_t tail = (dl == 8) ? 10 : 12;    // ROLE+TS(+PRI) + END
                uint8_t buf[32];
                std::memcpy(buf, hdr, 4);
                t=0;while(t<tail){size_t n=c.recv(buf+4+t,tail-t);if(n==0)break;t+=n;}
                if(t<tail)break;
                size_t total = 4 + tail;

                pmpc::redundancy::HeartbeatInfo info;
                if (pmpc::redundancy::ParseHeartbeatFrame(buf, total, info)) {
                    missedHeartbeats_ = 0;
                    peerRole_    = info.role;
                    peerPriority_ = info.priority;
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
    // 心跳丢失时先自增计数，让 DecideRole 拿到最新数值判断是否 promote。
    if(!peerAlive_) missedHeartbeats_++;
    RedundRole next = pmpc::redundancy::DecideRole(
        role_.load(), peerRole_.load(), peerAlive_.load(),
        missedHeartbeats_, missedHeartbeatLimit_,
        static_cast<uint16_t>(priority_), peerPriority_);
    if (next != role_.load()) {
        if (role_.load() == RedundRole::Master && next == RedundRole::Standby)
            std::cerr<<"[Redundancy] 双主检测，本站降级"<<std::endl;
        SetRole(next);
    }
}
void RedundancyManager::SetRole(RedundRole nr){
    // C5 修复：并发进入的两个 SetRole 应互相 serialize，且用同一把锁保护
    // 状态检查+转换，避免"两个线程都看到 role_==Idle 都启动 syncChannel_"
    // 之类的双启动。roleMtx_ 与 hbMtx_ 无嵌套：SetRole 不会持 roleMtx_ 时
    // 反向抓 hbMtx_，因为 syncChannel_.Start*/Stop 不使用 hbMtx_。
    std::lock_guard<std::mutex> lock(roleMtx_);
    if(role_==nr)return;
    RedundRole or_=role_;role_=nr;
    std::cout<<"\n*** "<<RoleName(or_)<<" -> "<<RoleName(nr)<<" ***\n"<<std::endl;
    // 切换角色前先 Stop 现有 syncChannel_（如果 Idle→X 就是空操作，本来就没启动）
    syncChannel_.Stop();
    if(nr==RedundRole::Master){syncChannel_.StartMaster(syncPort_);syncChannel_.SetChangeCallback(nullptr);}
    else if(nr==RedundRole::Standby){syncChannel_.StartStandby(peerIp_,syncPort_);
        syncChannel_.SetChangeCallback([this](const ChangeEvent&ev){OnSyncData((ChangeEvent::Type)ev.type,ev.channel,ev.device,ev.point,ev.value,ev.dvalue);});}
    // else Idle: Stop 已上面调过
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
